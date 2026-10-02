#include "IR/PDG/Analysis/BoundsQuery.h"

#include "llvm/ADT/Triple.h"
#include "llvm/Analysis/AssumptionCache.h"
#include "llvm/Analysis/BasicAliasAnalysis.h"
#include "llvm/Analysis/MemorySSA.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/GlobalAlias.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Operator.h"

#include "IR/PDG/Analysis/FunctionFacts.h"
#include "IR/PDG/Analysis/LibraryModels.h"
#include "IR/PDG/Analysis/ValueFacts.h"

#include <limits>
#include <map>
#include <set>

using namespace llvm;

namespace pdg {

bool BufferAccess::outOfBounds() const {
  // Zero-length operations need no dereference, including one-past pointers.
  if (!bytes)
    return false;
  if (region.offset < 0)
    return true;
  uint64_t offset = static_cast<uint64_t>(region.offset);
  return offset > region.capacity || bytes > region.capacity - offset;
}

namespace {
Optional<uint64_t> typeBytes(Type *type, const DataLayout &layout,
                             bool storage = true) {
  if (!type->isSized())
    return None;
  TypeSize size =
      storage ? layout.getTypeAllocSize(type) : layout.getTypeStoreSize(type);
  return size.isScalable() ? None : Optional<uint64_t>(size.getFixedValue());
}

bool characterArray(Type *type) {
  const auto *array = dyn_cast<ArrayType>(type);
  return array && array->getElementType()->isIntegerTy(8);
}

Optional<uint64_t> product(uint64_t a, uint64_t b) {
  if (a && b > std::numeric_limits<uint64_t>::max() / a)
    return None;
  return a * b;
}
} // namespace

struct BoundsQuery::Impl {
  struct FunctionState {
    TargetLibraryInfoImpl tli_impl;
    TargetLibraryInfo tli;
    AssumptionCache assumptions;
    DominatorTree dominators;
    BasicAAResult basic_aa;
    AAResults aliases;
    std::unique_ptr<MemorySSA> memory;
    FunctionFacts facts;

    explicit FunctionState(Function &function)
        : tli_impl(Triple(function.getParent()->getTargetTriple())),
          tli(tli_impl), assumptions(function), dominators(function),
          basic_aa(function.getParent()->getDataLayout(), function, tli,
                   assumptions, &dominators),
          aliases(tli), facts(function) {
      aliases.addAAResult(basic_aa);
      memory = std::make_unique<MemorySSA>(function, &aliases, &dominators);
    }
  };

  Module &module;
  const DataLayout &layout;
  mutable std::map<const Function *, std::unique_ptr<FunctionState>> states;

  explicit Impl(Module &m) : module(m), layout(m.getDataLayout()) {}

  FunctionState &state(const Function &function) const {
    auto &entry = states[&function];
    if (!entry)
      entry = std::make_unique<FunctionState>(const_cast<Function &>(function));
    return *entry;
  }

  const Value *loadValue(const LoadInst &load) const {
    if (load.isVolatile() || load.isAtomic())
      return nullptr;
    auto &analysis = state(*load.getFunction());
    MemoryAccess *clobber =
        analysis.memory->getWalker()->getClobberingMemoryAccess(&load);
    const auto *definition = dyn_cast<MemoryDef>(clobber);
    const auto *store =
        definition ? dyn_cast_or_null<StoreInst>(definition->getMemoryInst())
                   : nullptr;
    if (!store || store->isVolatile() || store->isAtomic() ||
        store->getValueOperand()->getType() != load.getType() ||
        !analysis.dominators.dominates(store, &load) ||
        analysis.aliases.alias(MemoryLocation::get(store),
                               MemoryLocation::get(&load)) !=
            AliasResult::MustAlias)
      return nullptr;
    return store->getValueOperand();
  }

  const Value *resolve(const Value *value, unsigned depth = 32) const {
    if (!depth)
      return value;
    if (const auto *load = dyn_cast<LoadInst>(value))
      if (const Value *stored = loadValue(*load))
        return resolve(stored, depth - 1);
    return value;
  }

  Optional<APInt> integer(const Value *value, unsigned depth = 32) const {
    if (!depth || !value->getType()->isIntegerTy())
      return None;
    value = resolve(value, depth);
    if (const auto *constant = dyn_cast<ConstantInt>(value))
      return constant->getValue();
    if (const auto *cast = dyn_cast<CastInst>(value)) {
      auto input = integer(cast->getOperand(0), depth - 1);
      if (!input)
        return None;
      unsigned width = cast->getType()->getIntegerBitWidth();
      if (cast->getOpcode() == Instruction::SExt)
        return input->sext(width);
      if (cast->getOpcode() == Instruction::ZExt)
        return input->zext(width);
      if (cast->getOpcode() == Instruction::Trunc)
        return input->trunc(width);
    }
    if (const auto *op = dyn_cast<BinaryOperator>(value)) {
      auto a = integer(op->getOperand(0), depth - 1);
      auto b = integer(op->getOperand(1), depth - 1);
      if (!a || !b)
        return None;
      if (op->getOpcode() == Instruction::Add)
        return *a + *b;
      if (op->getOpcode() == Instruction::Sub)
        return *a - *b;
      if (op->getOpcode() == Instruction::Mul)
        return *a * *b;
    }
    return None;
  }

  Optional<uint64_t> unsignedInteger(const Value *value) const {
    auto number = integer(value);
    if (!number || number->getActiveBits() > 64)
      return None;
    return number->getZExtValue();
  }

  Optional<MemoryRegion> region(const Value *value, unsigned depth = 48) const {
    if (!depth || !value->getType()->isPointerTy())
      return None;
    const Value *resolved = resolve(value);
    if (resolved != value)
      return region(resolved, depth - 1);
    if (const auto *alias = dyn_cast<GlobalAlias>(value))
      return region(alias->getAliasee(), depth - 1);
    if (const auto *cast = dyn_cast<BitCastOperator>(value))
      return region(cast->getOperand(0), depth - 1);
    if (const auto *cast = dyn_cast<AddrSpaceCastOperator>(value))
      return region(cast->getOperand(0), depth - 1);
    if (const auto *allocation = dyn_cast<AllocaInst>(value)) {
      auto size = typeBytes(allocation->getAllocatedType(), layout);
      auto count = unsignedInteger(allocation->getArraySize());
      if (!size || !count)
        return None;
      auto total = product(*size, *count);
      if (total)
        return MemoryRegion{value, *total, 0, false,
                            characterArray(allocation->getAllocatedType())};
      return None;
    }
    if (const auto *global = dyn_cast<GlobalVariable>(value)) {
      auto size = typeBytes(global->getValueType(), layout);
      if (size)
        return MemoryRegion{value, *size, 0, false,
                            characterArray(global->getValueType())};
      return None;
    }
    if (const auto *call = dyn_cast<CallBase>(value)) {
      const Function *target = ValueFacts::callee(*call);
      if (!target)
        return None;
      Optional<uint64_t> size;
      AllocationKind kind = LibraryModels::allocation(*target);
      if ((ValueFacts::hasLibraryName(*target, "malloc", true) ||
           kind == AllocationKind::New || kind == AllocationKind::NewArray) &&
          call->arg_size())
        size = unsignedInteger(call->getArgOperand(0));
      if (ValueFacts::hasLibraryName(*target, "calloc", true) &&
          call->arg_size() >= 2) {
        auto count = unsignedInteger(call->getArgOperand(0));
        auto element = unsignedInteger(call->getArgOperand(1));
        size = count && element ? product(*count, *element) : None;
      }
      if (ValueFacts::hasLibraryName(*target, "realloc", true) &&
          call->arg_size() >= 2)
        size = unsignedInteger(call->getArgOperand(1));
      if (ValueFacts::hasLibraryName(*target, "aligned_alloc", true) &&
          call->arg_size() >= 2)
        size = unsignedInteger(call->getArgOperand(1));
      if (size)
        return MemoryRegion{value, *size, 0, true, false};
      return None;
    }
    if (const auto *gep = dyn_cast<GEPOperator>(value)) {
      auto result = region(gep->getPointerOperand(), depth - 1);
      if (!result)
        return None;
      Type *type = gep->getSourceElementType();
      bool first = true;
      for (const Use &use : gep->indices()) {
        auto index = integer(use.get());
        if (!index || index->getMinSignedBits() > 64)
          return None;
        int64_t n = index->getSExtValue();
        if (!first && type->isStructTy()) {
          auto *structure = cast<StructType>(type);
          if (n < 0 || static_cast<uint64_t>(n) >= structure->getNumElements())
            return None;
          // Selecting a field narrows the region, preventing access from
          // silently crossing into a following struct member.
          auto enclosing = typeBytes(type, layout);
          if (!enclosing || result->offset < 0 ||
              static_cast<uint64_t>(result->offset) > result->capacity ||
              *enclosing >
                  result->capacity - static_cast<uint64_t>(result->offset))
            return None;
          const uint64_t fieldOffset =
              layout.getStructLayout(structure)->getElementOffset(n);
          const uint64_t remaining =
              result->capacity - static_cast<uint64_t>(result->offset);
          type = structure->getElementType(n);
          auto bytes = typeBytes(type, layout);
          if (!bytes)
            return None;
          // LLVM retains the trailing array shape but not the distinction
          // between a flexible member and the historical one-element idiom.
          // A zero-length tail stays unknown. An overallocated heap object
          // supplies the actual available extent of its one-element tail.
          const auto *array = dyn_cast<ArrayType>(type);
          bool last =
              static_cast<unsigned>(n) + 1 == structure->getNumElements();
          if (last && array && !array->getNumElements())
            return None;
          bool expandedTail = last && array && array->getNumElements() == 1 &&
                              result->heap && remaining > *enclosing;
          result->capacity = expandedTail ? remaining - fieldOffset : *bytes;
          result->offset = 0;
          result->static_character_array =
              !expandedTail && characterArray(type);
          continue;
        }
        Type *element = first               ? type
                        : type->isArrayTy() ? type->getArrayElementType()
                                            : nullptr;
        if (!element)
          return None;
        auto stride = typeBytes(element, layout);
        if (!stride)
          return None;
        APInt offset(128, static_cast<uint64_t>(result->offset), true);
        offset +=
            APInt(128, static_cast<uint64_t>(n), true) * APInt(128, *stride);
        if (offset.getMinSignedBits() > 64)
          return None;
        result->offset = offset.getSExtValue();
        if (!first && element->isArrayTy()) {
          // Only a valid outer index selects an inner array subobject.
          auto *outer = cast<ArrayType>(type);
          if (n < 0 || static_cast<uint64_t>(n) >= outer->getNumElements())
            return result;
          result->capacity = *stride;
          result->offset = 0;
          result->static_character_array = characterArray(element);
        }
        type = element;
        first = false;
      }
      return result;
    }
    // Require every alternative to identify the same region. Mixed origins
    // are unknown, rather than pretending that a may-origin is a proof.
    const auto *phi = dyn_cast<PHINode>(value);
    const auto *select = dyn_cast<SelectInst>(value);
    if (phi || select) {
      Optional<MemoryRegion> merged;
      unsigned count = phi ? phi->getNumIncomingValues() : 2;
      for (unsigned i = 0; i < count; ++i) {
        const Value *input = phi ? phi->getIncomingValue(i)
                             : i ? select->getFalseValue()
                                 : select->getTrueValue();
        auto incoming = region(input, depth - 1);
        if (!incoming)
          return None;
        if (merged && (merged->allocation != incoming->allocation ||
                       merged->capacity != incoming->capacity ||
                       merged->offset != incoming->offset))
          return None;
        merged = incoming;
      }
      return merged;
    }
    return None;
  }

  std::vector<BufferAccess> accesses(const Instruction &site) const {
    std::vector<BufferAccess> result;
    auto add = [&](const Value *pointer, Optional<uint64_t> bytes,
                   bool writes) {
      if (!pointer || !bytes)
        return;
      auto memory = region(pointer);
      if (memory)
        result.push_back({&site, pointer, *bytes, writes, *memory});
    };
    if (const auto *load = dyn_cast<LoadInst>(&site))
      add(load->getPointerOperand(), typeBytes(load->getType(), layout, false),
          false);
    if (const auto *store = dyn_cast<StoreInst>(&site))
      add(store->getPointerOperand(),
          typeBytes(store->getValueOperand()->getType(), layout, false), true);
    if (const auto *atomic = dyn_cast<AtomicRMWInst>(&site))
      add(atomic->getPointerOperand(),
          typeBytes(atomic->getValOperand()->getType(), layout, false), true);
    if (const auto *atomic = dyn_cast<AtomicCmpXchgInst>(&site))
      add(atomic->getPointerOperand(),
          typeBytes(atomic->getCompareOperand()->getType(), layout, false),
          true);
    if (const auto *transfer = dyn_cast<MemTransferInst>(&site)) {
      auto size = unsignedInteger(transfer->getLength());
      add(transfer->getRawDest(), size, true);
      add(transfer->getRawSource(), size, false);
      return result;
    }
    if (const auto *set = dyn_cast<MemSetInst>(&site)) {
      add(set->getRawDest(), unsignedInteger(set->getLength()), true);
      return result;
    }
    const auto *call = dyn_cast<CallBase>(&site);
    const Function *target = call ? ValueFacts::callee(*call) : nullptr;
    if (!target)
      return result;
    auto argument = [&](unsigned index) {
      return ValueFacts::argument(*call, index);
    };
    auto count = [&](unsigned index) -> Optional<uint64_t> {
      const Value *value = argument(index);
      return value ? unsignedInteger(value) : None;
    };
    auto named = [&](const char *name) {
      return ValueFacts::hasLibraryName(*target, name, true, true);
    };
    if (named("memcpy") || named("memmove") || named("mempcpy")) {
      add(argument(0), count(2), true);
      add(argument(1), count(2), false);
    } else if (named("bcopy")) {
      add(argument(1), count(2), true);
      add(argument(0), count(2), false);
    } else if (named("memset") || named("strncpy")) {
      add(argument(0), count(2), true);
    } else if (named("strncat")) {
      auto size = count(2);
      if (size && *size != std::numeric_limits<uint64_t>::max())
        add(argument(0), *size + 1, true);
    } else if (named("memcmp")) {
      add(argument(0), count(2), false);
      add(argument(1), count(2), false);
    } else if (named("read") || named("recv")) {
      add(argument(1), count(2), true);
    } else if (named("write") || named("send")) {
      add(argument(1), count(2), false);
    } else if (named("fgets")) {
      // fgets takes a signed int. A nonpositive count does not request a
      // large unsigned write, unlike the size_t parameters of memory APIs.
      const Value *size = argument(1);
      auto signedSize = size ? integer(size) : None;
      if (signedSize && !signedSize->isNegative())
        add(argument(0), count(1), true);
    } else if (named("snprintf") || named("vsnprintf")) {
      add(argument(0), count(1), true);
    } else if (named("fread") || named("fwrite")) {
      auto elements = count(2), size = count(1);
      add(argument(0), elements && size ? product(*elements, *size) : None,
          named("fread"));
    } else if (named("strcpy") || named("stpcpy")) {
      if (const Value *source = argument(1))
        if (auto text = ValueFacts::constantString(*resolve(source)))
          add(argument(0), text->size() + 1, true);
    }
    return result;
  }

  struct StringLength {
    const CallBase *call = nullptr;
    uint64_t scale = 1;
  };

  Optional<StringLength> lengthCall(const Value *value,
                                    unsigned depth = 32) const {
    if (!depth)
      return None;
    value = resolve(value);
    if (const auto *cast = dyn_cast<CastInst>(value)) {
      if (cast->getType()->isIntegerTy() &&
          cast->getOperand(0)->getType()->isIntegerTy())
        return lengthCall(cast->getOperand(0), depth - 1);
    }
    if (const auto *op = dyn_cast<BinaryOperator>(value)) {
      if (op->getOpcode() == Instruction::Mul)
        for (unsigned i = 0; i < 2; ++i) {
          auto constant = unsignedInteger(op->getOperand(i));
          auto length = lengthCall(op->getOperand(1 - i), depth - 1);
          if (constant && *constant && length)
            if (auto scale = product(length->scale, *constant))
              return StringLength{length->call, *scale};
        }
      if (op->getOpcode() == Instruction::Shl) {
        auto shift = unsignedInteger(op->getOperand(1));
        auto length = lengthCall(op->getOperand(0), depth - 1);
        if (shift && *shift < 64 && length)
          if (auto scale = product(length->scale, uint64_t(1) << *shift))
            return StringLength{length->call, *scale};
      }
    }
    const auto *call = dyn_cast<CallBase>(value);
    const Function *target = call ? ValueFacts::callee(*call) : nullptr;
    if (target && call->arg_size() == 1 &&
        (ValueFacts::hasLibraryName(*target, "strlen", true, true) ||
         ValueFacts::hasLibraryName(*target, "wcslen", true, true)))
      return StringLength{call, 1};
    return None;
  }

  const CallBase *allocationOrigin(const Value *pointer,
                                   unsigned depth = 32) const {
    if (!depth)
      return nullptr;
    pointer = resolve(pointer);
    if (const auto *cast = dyn_cast<BitCastOperator>(pointer))
      return allocationOrigin(cast->getOperand(0), depth - 1);
    if (const auto *gep = dyn_cast<GEPOperator>(pointer)) {
      APInt offset(layout.getIndexTypeSizeInBits(pointer->getType()), 0);
      if (!gep->accumulateConstantOffset(layout, offset) || !offset.isZero())
        return nullptr;
      return allocationOrigin(gep->getPointerOperand(), depth - 1);
    }
    const auto *call = dyn_cast<CallBase>(pointer);
    const Function *target = call ? ValueFacts::callee(*call) : nullptr;
    return target &&
                   LibraryModels::allocation(*target) != AllocationKind::Unknown
               ? call
               : nullptr;
  }

  Optional<StringLength> allocationLength(const CallBase &allocation) const {
    const Function *target = ValueFacts::callee(allocation);
    if (!target || !allocation.arg_size())
      return None;
    auto named = [&](const char *name) {
      return ValueFacts::hasLibraryName(*target, name, true);
    };
    auto kind = LibraryModels::allocation(*target);
    if (named("malloc") || kind == AllocationKind::New ||
        kind == AllocationKind::NewArray)
      return lengthCall(allocation.getArgOperand(0));
    if ((named("realloc") || named("aligned_alloc")) &&
        allocation.arg_size() >= 2)
      return lengthCall(allocation.getArgOperand(1));
    if (named("calloc") && allocation.arg_size() >= 2)
      for (unsigned i = 0; i < 2; ++i) {
        auto length = lengthCall(allocation.getArgOperand(i));
        auto multiplier = unsignedInteger(allocation.getArgOperand(1 - i));
        if (length && multiplier && *multiplier)
          if (auto scale = product(length->scale, *multiplier))
            return StringLength{length->call, *scale};
      }
    return None;
  }

  struct ScaledValue {
    const Value *value = nullptr;
    uint64_t scale = 1;
  };

  Optional<ScaledValue> scaled(const Value *value, const Instruction &at,
                               unsigned depth = 24) const {
    if (!depth || !value->getType()->isIntegerTy())
      return None;
    value = resolve(value);
    if (const auto *cast = dyn_cast<CastInst>(value)) {
      if (cast->getOpcode() == Instruction::ZExt ||
          (cast->getOpcode() == Instruction::SExt &&
           state(*at.getFunction())
               .facts.nonNegative(*cast->getOperand(0), at)))
        return scaled(cast->getOperand(0), at, depth - 1);
    }
    if (const auto *op = dyn_cast<BinaryOperator>(value)) {
      if (op->getOpcode() == Instruction::Mul)
        for (unsigned i = 0; i < 2; ++i) {
          auto factor = unsignedInteger(op->getOperand(i));
          auto base = scaled(op->getOperand(1 - i), at, depth - 1);
          if (factor && *factor && base)
            if (auto total = product(*factor, base->scale))
              return ScaledValue{base->value, *total};
        }
      if (op->getOpcode() == Instruction::Shl) {
        auto shift = unsignedInteger(op->getOperand(1));
        auto base = scaled(op->getOperand(0), at, depth - 1);
        if (shift && *shift < 64 && base)
          if (auto total = product(uint64_t(1) << *shift, base->scale))
            return ScaledValue{base->value, *total};
      }
    }
    return ScaledValue{value, 1};
  }

  bool allocationEqualsIndex(const CallBase &allocation, const Value &index,
                             uint64_t stride, const Instruction &at) const {
    const Function *target = ValueFacts::callee(allocation);
    if (!target || !allocation.arg_size())
      return false;
    auto kind = LibraryModels::allocation(*target);
    const Value *size = nullptr;
    uint64_t factor = 1;
    if (ValueFacts::hasLibraryName(*target, "malloc", true) ||
        kind == AllocationKind::New || kind == AllocationKind::NewArray)
      size = allocation.getArgOperand(0);
    if ((ValueFacts::hasLibraryName(*target, "realloc", true) ||
         ValueFacts::hasLibraryName(*target, "aligned_alloc", true)) &&
        allocation.arg_size() >= 2)
      size = allocation.getArgOperand(1);
    if (ValueFacts::hasLibraryName(*target, "calloc", true) &&
        allocation.arg_size() >= 2)
      for (unsigned i = 0; i < 2; ++i)
        if (auto element = unsignedInteger(allocation.getArgOperand(i))) {
          size = allocation.getArgOperand(1 - i);
          factor = *element;
          break;
        }
    if (!size || integer(size))
      return false; // Constant extents are handled by MemoryRegion.
    auto length = scaled(size, at), offset = scaled(&index, at);
    if (!length || !offset)
      return false;
    auto lengthScale = product(length->scale, factor);
    auto offsetScale = product(offset->scale, stride);
    return lengthScale && offsetScale && *lengthScale == *offsetScale &&
           state(*at.getFunction())
               .facts.equivalent(*length->value, *offset->value);
  }

  struct AllocationEnd {
    const CallBase *allocation = nullptr;
    int64_t delta = 0;
  };

  Optional<AllocationEnd> allocationEnd(const Value *pointer,
                                        const Instruction &at,
                                        unsigned depth = 32) const {
    if (!depth)
      return None;
    pointer = resolve(pointer);
    if (const auto *cast = dyn_cast<BitCastOperator>(pointer))
      return allocationEnd(cast->getOperand(0), at, depth - 1);
    const auto *gep = dyn_cast<GEPOperator>(pointer);
    if (!gep)
      return None;
    APInt displacement(layout.getIndexTypeSizeInBits(pointer->getType()), 0);
    if (gep->accumulateConstantOffset(layout, displacement)) {
      auto base = allocationEnd(gep->getPointerOperand(), at, depth - 1);
      if (!base || displacement.getMinSignedBits() > 64)
        return None;
      APInt delta(128, static_cast<uint64_t>(base->delta), true);
      delta += displacement.sextOrTrunc(128);
      if (delta.getMinSignedBits() > 64)
        return None;
      return AllocationEnd{base->allocation, delta.getSExtValue()};
    }
    // A dynamic pointer index is compared semantically to the byte count
    // allocated. It is a relative bound, not a guessed integer range.
    if (gep->getNumIndices() != 1)
      return None;
    const auto *allocation = allocationOrigin(gep->getPointerOperand());
    auto stride = typeBytes(gep->getSourceElementType(), layout);
    if (allocation && stride &&
        allocationEqualsIndex(*allocation, *gep->idx_begin()->get(), *stride,
                              at))
      return AllocationEnd{allocation, 0};
    return None;
  }

  std::vector<BoundsFinding> analyze() const {
    std::vector<BoundsFinding> result;
    std::set<std::pair<std::string, const Instruction *>> emitted;
    auto emit = [&](StringRef id, const Instruction *site, std::string message,
                    std::vector<const Value *> evidence) {
      if (emitted.insert({id.str(), site}).second) {
        std::vector<const Instruction *> instructions;
        for (const Value *value : evidence)
          if (const auto *instruction = dyn_cast<Instruction>(value))
            instructions.push_back(instruction);
        result.push_back(
            {id.str(), site, std::move(message), std::move(instructions)});
      }
    };
    for (const Function &function : module) {
      if (function.isDeclaration())
        continue;
      auto &analysis = state(function);
      for (const BasicBlock &block : function) {
        if (!analysis.dominators.isReachableFromEntry(&block))
          continue;
        for (const Instruction &inst : block) {
          for (const BufferAccess &access : accesses(inst)) {
            if (!access.outOfBounds())
              continue;
            std::string message =
                std::string(access.writes ? "Write" : "Read") + " of " +
                std::to_string(access.bytes) + " bytes at byte offset " +
                std::to_string(access.region.offset) +
                " exceeds the object's " +
                std::to_string(access.region.capacity) + " byte region.";
            std::vector<const Value *> evidence{access.region.allocation,
                                                access.pointer};
            emit("cpp/overflow-buffer", &inst, message, evidence);
            if (access.region.static_character_array &&
                access.region.offset >= 0)
              emit("cpp/static-buffer-overflow", &inst, message, evidence);
            if (access.region.heap &&
                (isa<LoadInst>(inst) || isa<StoreInst>(inst)))
              emit("cpp/invalid-pointer-deref", &inst, message, evidence);
          }
          const Value *dereferenced = nullptr;
          Type *accessType = nullptr;
          if (const auto *load = dyn_cast<LoadInst>(&inst)) {
            dereferenced = load->getPointerOperand();
            accessType = load->getType();
          } else if (const auto *store = dyn_cast<StoreInst>(&inst)) {
            dereferenced = store->getPointerOperand();
            accessType = store->getValueOperand()->getType();
          }
          if (dereferenced) {
            auto end = allocationEnd(dereferenced, inst);
            auto bytes = typeBytes(accessType, layout, false);
            if (end && bytes && *bytes) {
              APInt accessed(128, static_cast<uint64_t>(end->delta), true);
              accessed += APInt(128, *bytes);
              if (accessed.isStrictlyPositive())
                emit("cpp/invalid-pointer-deref", &inst,
                     "This dereference reaches beyond the allocation's "
                     "symbolic byte extent (offset from its end: " +
                         std::to_string(end->delta) + " bytes).",
                     {end->allocation, dereferenced});
            }
          }
          const auto *copy = dyn_cast<CallBase>(&inst);
          const Function *target = copy ? ValueFacts::callee(*copy) : nullptr;
          if (!target || copy->arg_size() < 2)
            continue;
          bool stringCopy =
              ValueFacts::hasLibraryName(*target, "strcpy", true, true) ||
              ValueFacts::hasLibraryName(*target, "stpcpy", true, true) ||
              ValueFacts::hasLibraryName(*target, "wcscpy", true, true) ||
              ValueFacts::hasLibraryName(*target, "strcat", true, true) ||
              ValueFacts::hasLibraryName(*target, "wcscat", true, true);
          const Value *copySource =
              stringCopy ? copy->getArgOperand(1) : nullptr;
          if (ValueFacts::hasLibraryName(*target, "sprintf", true, true) &&
              copy->arg_size() == 3) {
            auto format = ValueFacts::constantString(*copy->getArgOperand(1));
            if (format && *format == "%s")
              copySource = copy->getArgOperand(2);
          }
          if (!copySource)
            continue;
          const auto *allocation = allocationOrigin(copy->getArgOperand(0));
          auto length = allocation ? allocationLength(*allocation) : None;
          if (!length || !analysis.dominators.dominates(allocation, copy))
            continue;
          const Value *source = resolve(copySource);
          const Value *measured = resolve(length->call->getArgOperand(0));
          // wcslen counts characters; malloc counts bytes. Use the target
          // pointee type rather than assuming a host wchar_t width.
          auto width =
              measured->getType()->isPointerTy()
                  ? typeBytes(measured->getType()->getPointerElementType(),
                              layout)
                  : None;
          if (!width || length->scale > *width)
            continue;
          if (analysis.facts.equivalent(*source, *measured))
            emit("cpp/no-space-for-terminator", allocation,
                 "This string-length-sized allocation has no space reserved "
                 "for the "
                 "terminating zero written by the string copy.",
                 {length->call, copy, source});
        }
      }
    }
    return result;
  }
};

BoundsQuery::BoundsQuery() = default;
BoundsQuery::BoundsQuery(Module &module) : impl_(new Impl(module)) {}
BoundsQuery::~BoundsQuery() = default;
Optional<MemoryRegion> BoundsQuery::region(const Value &pointer) const {
  return impl_ ? impl_->region(&pointer) : None;
}
std::vector<BufferAccess> BoundsQuery::accesses(const Instruction &site) const {
  return impl_ ? impl_->accesses(site) : std::vector<BufferAccess>();
}
std::vector<BoundsFinding> BoundsQuery::analyze() const {
  return impl_ ? impl_->analyze() : std::vector<BoundsFinding>();
}

const std::vector<BoundsRuleDescriptor> &BoundsQuery::catalog() {
  static const std::vector<BoundsRuleDescriptor> rules{
      {"cpp/overflow-buffer", "warning",
       "Security/CWE/CWE-119/OverflowBuffer.ql",
       "Known object/subobject capacity, constant byte offset and access size; "
       "loads/stores/atomics, memory intrinsics, and modeled C buffer APIs. "
       "Local pointer spills use must-alias dominating MemorySSA definitions. "
       "Dynamic indices and interprocedural pointer origins remain unknown."},
      {"cpp/static-buffer-overflow", "warning", "Critical/OverflowStatic.ql",
       "Statically sized char arrays and char array members with constant "
       "out-of-bounds dereferences or overlarge modeled API lengths. Loop "
       "induction and source macro exclusions are not yet modeled."},
      {"cpp/invalid-pointer-deref", "error",
       "Security/CWE/CWE-193/InvalidPointerDeref.ql",
       "Load/store of known-size malloc/calloc/realloc/new regions using "
       "constant out-of-bounds offsets, plus symbolic allocation-size equals "
       "pointer-offset relations with matching byte/element scaling. Constant "
       "offsets from that end are supported. Constructing a one-past pointer "
       "is not reported. Loop ranges and cross-call pointer flow remain "
       "unknown."},
      {"cpp/no-space-for-terminator", "error",
       "Security/CWE/CWE-131/NoSpaceForZeroTerminator.ql",
       "malloc/calloc/realloc/aligned_alloc/new with strlen/wcslen-derived "
       "size and no terminator element, followed by a same-source strcpy/"
       "stpcpy/wcscpy/strcat/wcscat or sprintf(\"%s\"). Byte/character scaling "
       "uses the target pointee type. Integer casts and unmodified local "
       "spills are followed; other consumers and cross-call flow remain "
       "unknown."}};
  return rules;
}

BoundsQueryResult BoundsQuery::analyze(const Module &module) const {
  BoundsQuery query(const_cast<Module &>(module));
  return {query.analyze()};
}

} // namespace pdg
