#include "Alias/InclusionBased/GPG/LLVMFrontend.h"

#include <algorithm>
#include <deque>
#include <iterator>
#include <utility>

#include <llvm/IR/CFG.h>
#include <llvm/IR/Constant.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GetElementPtrTypeIterator.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Operator.h>

namespace lotus::gpg {

namespace {

bool isMemoryCopy(const llvm::Function *function) {
  if (!function)
    return false;
  return function->getName().startswith("llvm.memcpy") ||
         function->getName().startswith("llvm.memmove");
}

bool isMemorySet(const llvm::Function *function) {
  return function && function->getName().startswith("llvm.memset");
}

bool isIgnoredIntrinsic(const llvm::Function *function) {
  if (!function || !function->isIntrinsic())
    return false;
  switch (function->getIntrinsicID()) {
  case llvm::Intrinsic::lifetime_start:
  case llvm::Intrinsic::lifetime_end:
  case llvm::Intrinsic::dbg_declare:
  case llvm::Intrinsic::dbg_value:
  case llvm::Intrinsic::assume:
    return true;
  default:
    return false;
  }
}

bool isAggregate(const llvm::Type *type) {
  return type &&
         (type->isStructTy() || type->isArrayTy() || type->isVectorTy());
}

} // namespace

LLVMFrontend::LLVMFrontend(ProgramModel &model, const GPGConfig &config)
    : model_(model), config_(config) {
  collectEntryReachableBlocks();
  collectGlobalInitializers();
}

std::vector<Access> LLVMFrontend::readAccesses(const llvm::Value *value) {
  if (!value)
    return {model_.access(model_.unknownLocation(), {})};

  auto cached = read_access_cache_.find(value);
  if (cached != read_access_cache_.end())
    return refreshKLimiting(cached->second);

  if (resolving_values_.count(value) != 0) {
    markResolutionCycle(value);
    return fallbackReadAccesses(value);
  }

  resolving_values_[value] = resolution_stack_.size();
  resolution_stack_.push_back(value);
  std::vector<Access> result = computeReadAccesses(value);
  resolution_stack_.pop_back();
  resolving_values_.erase(value);

  if (result.empty())
    result = fallbackReadAccesses(value);
  std::set<Access> unique(result.begin(), result.end());
  result.assign(unique.begin(), unique.end());
  result = refreshKLimiting(std::move(result));
  read_access_cache_[value] = result;
  return result;
}

std::vector<Access>
LLVMFrontend::computeReadAccesses(const llvm::Value *value) {
  if (!value)
    return {model_.access(model_.unknownLocation(), {})};

  if (auto *function = llvm::dyn_cast<llvm::Function>(value)) {
    return {model_.access(model_.functionLocation(function), {})};
  }
  if (auto *global = llvm::dyn_cast<llvm::GlobalVariable>(value)) {
    return {model_.access(model_.objectLocation(global), {})};
  }
  if (auto *alias = llvm::dyn_cast<llvm::GlobalAlias>(value))
    return readAccesses(alias->getAliasee());
  if (llvm::isa<llvm::ConstantPointerNull>(value))
    return {model_.access(model_.nullLocation(), {})};
  if (llvm::isa<llvm::UndefValue>(value) || llvm::isa<llvm::PoisonValue>(value))
    return {model_.access(model_.unknownLocation(), {})};

  if (auto *alloca = llvm::dyn_cast<llvm::AllocaInst>(value))
    return {model_.access(model_.objectLocation(alloca), {})};
  if (auto *gep = llvm::dyn_cast<llvm::GetElementPtrInst>(value)) {
    bool pointer_arithmetic = false;
    std::vector<Indirection> path = gepPath(*gep, pointer_arithmetic);
    std::vector<Access> result;
    for (const Access &base : readAccesses(gep->getPointerOperand())) {
      Access resolved = append(base, path);
      if (pointer_arithmetic)
        recordPointerArithmetic(resolved);
      result.push_back(std::move(resolved));
    }
    return result;
  }
  if (auto *cast = llvm::dyn_cast<llvm::CastInst>(value)) {
    if (cast->getType()->isPointerTy() &&
        cast->getOperand(0)->getType()->isPointerTy())
      return readAccesses(cast->getOperand(0));
  }
  if (auto *freeze = llvm::dyn_cast<llvm::FreezeInst>(value)) {
    if (freeze->getType()->isPointerTy())
      return readAccesses(freeze->getOperand(0));
  }
  if (auto *phi = llvm::dyn_cast<llvm::PHINode>(value)) {
    if (phi->getType()->isPointerTy()) {
      std::vector<Access> result;
      for (unsigned index = 0; index < phi->getNumIncomingValues(); ++index) {
        if (entry_reachable_blocks_.count(phi->getIncomingBlock(index)) == 0)
          continue;
        std::vector<Access> resolved =
            readAccesses(phi->getIncomingValue(index));
        result.insert(result.end(), resolved.begin(), resolved.end());
      }
      return result;
    }
  }
  if (auto *select = llvm::dyn_cast<llvm::SelectInst>(value)) {
    if (select->getType()->isPointerTy()) {
      std::vector<Access> result = readAccesses(select->getTrueValue());
      std::vector<Access> false_values = readAccesses(select->getFalseValue());
      result.insert(result.end(), false_values.begin(), false_values.end());
      return result;
    }
  }
  if (auto *call = llvm::dyn_cast<llvm::CallBase>(value)) {
    if (model_.findObjectLocation(call))
      return {model_.access(model_.objectLocation(call), {})};
  }

  if (auto *constant = llvm::dyn_cast<llvm::ConstantExpr>(value)) {
    if (constant->isCast())
      return readAccesses(constant->getOperand(0));
    if (constant->getOpcode() == llvm::Instruction::GetElementPtr) {
      auto *gep = llvm::cast<llvm::GEPOperator>(constant);
      bool pointer_arithmetic = false;
      std::vector<Indirection> path = gepPath(*gep, pointer_arithmetic);
      std::vector<Access> result;
      for (const Access &base : readAccesses(gep->getPointerOperand())) {
        Access resolved = append(base, path);
        if (pointer_arithmetic)
          recordPointerArithmetic(resolved);
        result.push_back(std::move(resolved));
      }
      return result;
    }
  }

  if (value->getType()->isPointerTy()) {
    return {model_.access(model_.valueLocation(value),
                          IndirectionList::dereferences(1))};
  }
  return {model_.access(model_.unknownLocation(), {})};
}

std::vector<Access>
LLVMFrontend::fallbackReadAccesses(const llvm::Value *value) {
  if (value && value->getType()->isPointerTy()) {
    return {model_.access(model_.valueLocation(value),
                          IndirectionList::dereferences(1))};
  }
  return {model_.access(model_.unknownLocation(), {})};
}

void LLVMFrontend::markResolutionCycle(const llvm::Value *value) {
  auto found = resolving_values_.find(value);
  if (found == resolving_values_.end())
    return;
  for (std::size_t index = found->second; index < resolution_stack_.size();
       ++index) {
    const llvm::Value *cyclic_value = resolution_stack_[index];
    if (cyclic_value->getType()->isPointerTy()) {
      model_.markRequiresKLimiting(model_.valueLocation(cyclic_value));
    }
  }
}

std::vector<Access>
LLVMFrontend::refreshKLimiting(std::vector<Access> accesses) const {
  for (Access &access : accesses)
    access.k_limited |= model_.requiresKLimiting(access.location);
  return accesses;
}

void LLVMFrontend::recordPointerArithmetic(const Access &access) {
  auto &paths = pointer_arithmetic_paths_[access.location];
  if (std::find(paths.begin(), paths.end(), access.indirections) == paths.end())
    paths.push_back(access.indirections);
}

bool LLVMFrontend::isPointerArithmeticAccess(const Access &access) const {
  auto found = pointer_arithmetic_paths_.find(access.location);
  if (found == pointer_arithmetic_paths_.end())
    return false;
  return std::any_of(found->second.begin(), found->second.end(),
                     [&](const IndirectionList &path) {
                       return path.isPrefixOf(access.indirections);
                     });
}

void LLVMFrontend::collectEntryReachableBlocks() {
  for (llvm::Function &function : model_.module()) {
    if (function.empty())
      continue;
    std::deque<const llvm::BasicBlock *> worklist;
    worklist.push_back(&function.getEntryBlock());
    while (!worklist.empty()) {
      const llvm::BasicBlock *block = worklist.front();
      worklist.pop_front();
      if (!entry_reachable_blocks_.insert(block).second)
        continue;
      for (const llvm::BasicBlock *successor : llvm::successors(block))
        worklist.push_back(successor);
    }
  }
}

Access LLVMFrontend::append(const Access &base,
                            const std::vector<Indirection> &suffix) const {
  IndirectionList path = base.indirections.append(
      IndirectionList(suffix),
      base.k_limited ? config_.heap_indirection_limit : 0);
  Access result =
      model_.access(base.location, std::move(path), base.upward_exposed);
  result.k_limited |= base.k_limited;
  return result;
}

Access LLVMFrontend::appendDereference(const Access &base) const {
  return append(base, {Indirection::dereference()});
}

std::optional<Access> LLVMFrontend::valueQueryAccess(const llvm::Value *value) {
  if (!value)
    return std::nullopt;
  if (const MemoryLocation *location = model_.findValueLocation(value)) {
    return model_.access(location->id, IndirectionList::dereferences(1));
  }
  if (const MemoryLocation *location = model_.findObjectLocation(value))
    return model_.access(location->id, {});
  if (value->getType()->isPointerTy()) {
    return model_.access(model_.valueLocation(value),
                         IndirectionList::dereferences(1));
  }
  return std::nullopt;
}

GPU LLVMFrontend::makeGPU(const Access &source, const Access &target,
                          const llvm::Instruction &instruction, GPUKind kind) {
  GPU result;
  result.source = source;
  result.target = target;
  result.source.k_limited |= model_.requiresKLimiting(result.source.location);
  result.target.k_limited |= model_.requiresKLimiting(result.target.location);
  result.statement = model_.statementId(&instruction);
  result.kind = kind;
  result.origin = &instruction;
  result.source_may_alias_multiple =
      model_.forcesWeakUpdate(source.location) ||
      std::any_of(source.indirections.elements().begin(),
                  source.indirections.elements().end(),
                  [](const Indirection &step) {
                    return step.kind == IndirectionKind::AnyField;
                  });
  const MemoryLocation *source_location = model_.getLocation(source.location);
  const bool memory_snapshot =
      llvm::isa<llvm::LoadInst>(instruction) ||
      llvm::isa<llvm::AtomicCmpXchgInst>(instruction) ||
      llvm::isa<llvm::AtomicRMWInst>(instruction) ||
      llvm::isa<llvm::VAArgInst>(instruction);
  const bool defines_ssa_value = source_location &&
                                 source_location->kind == LocationKind::SSA &&
                                 source_location->value == &instruction;
  const bool summarized_source =
      source_location && source_location->kind == LocationKind::Unknown;
  const bool heap_points_to = source_location &&
                              source_location->kind == LocationKind::Heap &&
                              result.isPointsToEdge();
  result.pointer_arithmetic = isPointerArithmeticAccess(result.source);
  result.flow_insensitive =
      !memory_snapshot &&
      (defines_ssa_value || model_.isArrayAccess(result.source) ||
       result.pointer_arithmetic || heap_points_to || summarized_source);
  result.provenance = next_gpu_provenance_++;
  if (source.indirections.size() > 1)
    result.queries.insert({source, QueryEndpoint::Source});
  if (!target.indirections.empty())
    result.queries.insert({target, QueryEndpoint::Target});
  if (defines_ssa_value)
    result.queries.insert({source, QueryEndpoint::Target});
  return result;
}

GPU LLVMFrontend::makeUseGPU(const Access &target,
                             const llvm::Instruction &instruction,
                             const llvm::Value *original_value) {
  Access source =
      model_.access(model_.useLocation(), IndirectionList::dereferences(1));
  GPU result = makeGPU(source, target, instruction, GPUKind::Use);
  result.queries.insert({target, QueryEndpoint::Target});
  if (std::optional<Access> original = valueQueryAccess(original_value))
    result.queries.insert({*original, QueryEndpoint::Target});
  return result;
}

std::vector<Indirection> LLVMFrontend::gepPath(const llvm::User &gep,
                                               bool &pointer_arithmetic) const {
  std::vector<Indirection> path;
  bool first_index = true;
  for (auto iterator = llvm::gep_type_begin(&gep),
            end = llvm::gep_type_end(&gep);
       iterator != end; ++iterator) {
    const llvm::Value *index = iterator.getOperand();
    if (iterator.isStruct()) {
      auto *constant = llvm::dyn_cast<llvm::ConstantInt>(index);
      if (!config_.field_sensitive || !constant) {
        path.push_back(Indirection::anyField());
        pointer_arithmetic |= !constant;
      } else {
        path.push_back(Indirection::fieldAt(
            static_cast<std::int64_t>(constant->getZExtValue())));
      }
      first_index = false;
      continue;
    }

    auto *constant = llvm::dyn_cast<llvm::ConstantInt>(index);
    if (first_index && constant && constant->isZero()) {
      first_index = false;
      continue;
    }
    first_index = false;
    if (config_.array_index_sensitive && constant) {
      path.push_back(Indirection::fieldAt(
          static_cast<std::int64_t>(constant->getSExtValue())));
    } else {
      path.push_back(Indirection::anyField());
    }
    pointer_arithmetic |= !constant || !constant->isZero();
  }
  return path;
}

void LLVMFrontend::addPointerOperandUses(GPB &block,
                                         llvm::Instruction &instruction) {
  for (llvm::Use &operand_use : instruction.operands()) {
    llvm::Value *operand = operand_use.get();
    if (!operand->getType()->isPointerTy())
      continue;
    for (const Access &target : readAccesses(operand))
      block.gpus.insert(makeUseGPU(target, instruction, operand));
  }
}

std::optional<GPB>
LLVMFrontend::translateInstruction(llvm::Instruction &instruction, GPBId id) {
  GPB block;
  block.id = id;
  block.origin_block = instruction.getParent();

  if (auto *alloca = llvm::dyn_cast<llvm::AllocaInst>(&instruction)) {
    Access source = model_.access(model_.valueLocation(alloca),
                                  IndirectionList::dereferences(1));
    Access target = model_.access(model_.objectLocation(alloca), {});
    block.gpus.insert(makeGPU(source, target, instruction));
  } else if (auto *gep =
                 llvm::dyn_cast<llvm::GetElementPtrInst>(&instruction)) {
    Access source = model_.access(model_.valueLocation(gep),
                                  IndirectionList::dereferences(1));
    bool pointer_arithmetic = false;
    std::vector<Indirection> path = gepPath(*gep, pointer_arithmetic);
    for (const Access &base : readAccesses(gep->getPointerOperand())) {
      Access target = append(base, path);
      if (pointer_arithmetic)
        recordPointerArithmetic(target);
      GPU update = makeGPU(source, target, instruction);
      update.pointer_arithmetic = pointer_arithmetic;
      block.gpus.insert(std::move(update));
    }
  } else if (auto *load = llvm::dyn_cast<llvm::LoadInst>(&instruction)) {
    for (const Access &pointer : readAccesses(load->getPointerOperand())) {
      Access target = appendDereference(pointer);
      if (load->getType()->isPointerTy()) {
        Access source = model_.access(model_.valueLocation(load),
                                      IndirectionList::dereferences(1));
        GPU update = makeGPU(source, target, instruction);
        update.flow_insensitive = false;
        block.gpus.insert(std::move(update));
      } else {
        block.gpus.insert(
            makeUseGPU(target, instruction, load->getPointerOperand()));
      }
    }
  } else if (auto *store = llvm::dyn_cast<llvm::StoreInst>(&instruction)) {
    std::vector<Access> destinations = readAccesses(store->getPointerOperand());
    if (store->getValueOperand()->getType()->isPointerTy()) {
      for (const Access &destination : destinations) {
        Access source = appendDereference(destination);
        for (const Access &target : readAccesses(store->getValueOperand())) {
          GPU update = makeGPU(source, target, instruction);
          if (std::optional<Access> original =
                  valueQueryAccess(store->getValueOperand())) {
            update.queries.insert({*original, QueryEndpoint::Target});
          }
          block.gpus.insert(std::move(update));
        }
      }
    } else {
      for (const Access &destination : destinations)
        block.gpus.insert(makeUseGPU(appendDereference(destination),
                                     instruction, store->getPointerOperand()));
    }
  } else if (auto *phi = llvm::dyn_cast<llvm::PHINode>(&instruction)) {
    if (!phi->getType()->isPointerTy())
      return std::nullopt;
    Access source = model_.access(model_.valueLocation(phi),
                                  IndirectionList::dereferences(1));
    for (unsigned index = 0; index < phi->getNumIncomingValues(); ++index) {
      if (entry_reachable_blocks_.count(phi->getIncomingBlock(index)) == 0)
        continue;
      for (const Access &target : readAccesses(phi->getIncomingValue(index)))
        block.gpus.insert(makeGPU(source, target, instruction));
    }
  } else if (auto *select = llvm::dyn_cast<llvm::SelectInst>(&instruction)) {
    if (!select->getType()->isPointerTy()) {
      addPointerOperandUses(block, instruction);
    } else {
      Access source = model_.access(model_.valueLocation(select),
                                    IndirectionList::dereferences(1));
      for (llvm::Value *value :
           {select->getTrueValue(), select->getFalseValue()}) {
        for (const Access &target : readAccesses(value))
          block.gpus.insert(makeGPU(source, target, instruction));
      }
    }
  } else if (auto *freeze = llvm::dyn_cast<llvm::FreezeInst>(&instruction)) {
    if (freeze->getType()->isPointerTy()) {
      Access source = model_.access(model_.valueLocation(freeze),
                                    IndirectionList::dereferences(1));
      for (const Access &target : readAccesses(freeze->getOperand(0)))
        block.gpus.insert(makeGPU(source, target, instruction));
    }
  } else if (auto *cast = llvm::dyn_cast<llvm::CastInst>(&instruction)) {
    if (cast->getType()->isPointerTy()) {
      Access source = model_.access(model_.valueLocation(cast),
                                    IndirectionList::dereferences(1));
      if (cast->getOperand(0)->getType()->isPointerTy()) {
        for (const Access &target : readAccesses(cast->getOperand(0)))
          block.gpus.insert(makeGPU(source, target, instruction));
      } else {
        Access target = model_.access(model_.unknownLocation(), {});
        GPU update = makeGPU(source, target, instruction);
        update.source_may_alias_multiple = true;
        block.gpus.insert(std::move(update));
      }
    } else {
      addPointerOperandUses(block, instruction);
    }
  } else if (auto *call = llvm::dyn_cast<llvm::CallBase>(&instruction)) {
    llvm::Function *callee = call->getCalledFunction();
    if (isIgnoredIntrinsic(callee))
      return std::nullopt;

    block.callsite = call;
    if (isMemoryCopy(callee) && call->arg_size() >= 2) {
      for (const Access &destination : readAccesses(call->getArgOperand(0))) {
        for (const Access &source : readAccesses(call->getArgOperand(1))) {
          GPU update = makeGPU(appendDereference(destination),
                               appendDereference(source), instruction);
          update.source_may_alias_multiple = true;
          block.gpus.insert(std::move(update));
        }
      }
    } else if (isMemorySet(callee) && call->arg_size() >= 2) {
      auto *value = llvm::dyn_cast<llvm::ConstantInt>(call->getArgOperand(1));
      Access target = value && value->isZero()
                          ? model_.access(model_.nullLocation(), {})
                          : model_.access(model_.unknownLocation(), {});
      for (const Access &destination : readAccesses(call->getArgOperand(0))) {
        GPU update =
            makeGPU(appendDereference(destination), target, instruction);
        update.source_may_alias_multiple = true;
        block.gpus.insert(std::move(update));
      }
    } else if (model_.findObjectLocation(call)) {
      Access source = model_.access(model_.valueLocation(call),
                                    IndirectionList::dereferences(1));
      Access target = model_.access(model_.objectLocation(call), {});
      block.gpus.insert(makeGPU(source, target, instruction));
    } else if (callee && !callee->isDeclaration()) {
      block.kind = GPBKind::DirectCall;
      block.callees.insert(callee);
    } else if (!callee) {
      block.kind = GPBKind::IndirectCall;
      for (const Access &target : readAccesses(call->getCalledOperand()))
        block.gpus.insert(
            makeUseGPU(target, instruction, call->getCalledOperand()));
    } else {
      // Unknown external bodies are modeled conservatively. Pointer results
      // may denote any location and pointer arguments may be modified.
      if (call->getType()->isPointerTy()) {
        Access source = model_.access(model_.valueLocation(call),
                                      IndirectionList::dereferences(1));
        GPU update = makeGPU(
            source, model_.access(model_.unknownLocation(), {}), instruction);
        update.source_may_alias_multiple = true;
        block.gpus.insert(std::move(update));
      }
      for (llvm::Value *argument : call->args()) {
        if (!argument->getType()->isPointerTy())
          continue;
        for (const Access &value : readAccesses(argument)) {
          GPU update =
              makeGPU(appendDereference(value),
                      model_.access(model_.unknownLocation(), {}), instruction);
          update.source_may_alias_multiple = true;
          block.gpus.insert(std::move(update));
        }
      }
    }
  } else if (auto *compare_exchange =
                 llvm::dyn_cast<llvm::AtomicCmpXchgInst>(&instruction)) {
    if (compare_exchange->getNewValOperand()->getType()->isPointerTy()) {
      for (const Access &pointer :
           readAccesses(compare_exchange->getPointerOperand())) {
        Access memory = appendDereference(pointer);
        for (const Access &target :
             readAccesses(compare_exchange->getNewValOperand())) {
          GPU update = makeGPU(memory, target, instruction);
          update.source_may_alias_multiple = true;
          update.flow_insensitive = false;
          block.gpus.insert(std::move(update));
        }
        Access aggregate =
            model_.access(model_.valueLocation(compare_exchange), {});
        Access old_value = append(
            aggregate, {Indirection::fieldAt(0), Indirection::dereference()});
        GPU old_update = makeGPU(old_value, memory, instruction);
        old_update.flow_insensitive = false;
        block.gpus.insert(std::move(old_update));
      }
    }
  } else if (auto *atomic = llvm::dyn_cast<llvm::AtomicRMWInst>(&instruction)) {
    if (atomic->getValOperand()->getType()->isPointerTy()) {
      Access result = model_.access(model_.valueLocation(atomic),
                                    IndirectionList::dereferences(1));
      for (const Access &pointer : readAccesses(atomic->getPointerOperand())) {
        Access memory = appendDereference(pointer);
        GPU old_update = makeGPU(result, memory, instruction);
        old_update.flow_insensitive = false;
        block.gpus.insert(std::move(old_update));
        for (const Access &target : readAccesses(atomic->getValOperand())) {
          GPU new_update = makeGPU(memory, target, instruction);
          new_update.flow_insensitive = false;
          block.gpus.insert(std::move(new_update));
        }
      }
    }
  } else if (auto *ret = llvm::dyn_cast<llvm::ReturnInst>(&instruction)) {
    if (llvm::Value *value = ret->getReturnValue()) {
      if (value->getType()->isPointerTy()) {
        Access source =
            model_.access(model_.returnLocation(instruction.getFunction()),
                          IndirectionList::dereferences(1));
        for (const Access &target : readAccesses(value))
          block.gpus.insert(
              makeGPU(source, target, instruction, GPUKind::Return));
      }
    }
  } else if (auto *extract =
                 llvm::dyn_cast<llvm::ExtractValueInst>(&instruction)) {
    if (extract->getType()->isPointerTy()) {
      Access source = model_.access(model_.valueLocation(extract),
                                    IndirectionList::dereferences(1));
      Access aggregate = model_.access(
          model_.valueLocation(extract->getAggregateOperand()), {});
      std::vector<Indirection> path;
      for (unsigned index : extract->getIndices())
        path.push_back(Indirection::fieldAt(index));
      path.push_back(Indirection::dereference());
      block.gpus.insert(makeGPU(source, append(aggregate, path), instruction));
    }
  } else if (auto *insert =
                 llvm::dyn_cast<llvm::InsertValueInst>(&instruction)) {
    llvm::Value *inserted = insert->getInsertedValueOperand();
    if (inserted->getType()->isPointerTy()) {
      Access aggregate = model_.access(model_.valueLocation(insert), {});
      std::vector<Indirection> path;
      for (unsigned index : insert->getIndices())
        path.push_back(Indirection::fieldAt(index));
      path.push_back(Indirection::dereference());
      Access source = append(aggregate, path);
      for (const Access &target : readAccesses(inserted))
        block.gpus.insert(makeGPU(source, target, instruction));
    }
  } else if (instruction.getType()->isPointerTy()) {
    Access source = model_.access(model_.valueLocation(&instruction),
                                  IndirectionList::dereferences(1));
    GPU update = makeGPU(source, model_.access(model_.unknownLocation(), {}),
                         instruction);
    update.source_may_alias_multiple = true;
    block.gpus.insert(std::move(update));
    addPointerOperandUses(block, instruction);
  } else {
    addPointerOperandUses(block, instruction);
  }

  if (block.gpus.empty() && block.kind == GPBKind::Normal)
    return std::nullopt;
  block.original_gpus = block.gpus;
  return block;
}

GPG LLVMFrontend::buildInitialGPG(llvm::Function &function) {
  GPG graph;
  GPBId next_id = 1;
  GPB start;
  start.id = next_id++;
  start.kind = GPBKind::Start;
  graph.addBlock(start);
  graph.setEntry(start.id);

  std::map<const llvm::BasicBlock *, GPBId> first;
  std::map<const llvm::BasicBlock *, GPBId> last;
  for (llvm::BasicBlock &basic_block : function) {
    if (entry_reachable_blocks_.count(&basic_block) == 0)
      continue;
    std::vector<GPBId> sequence;
    for (llvm::Instruction &instruction : basic_block) {
      std::optional<GPB> translated =
          translateInstruction(instruction, next_id);
      if (!translated)
        continue;
      sequence.push_back(next_id++);
      graph.addBlock(std::move(*translated));
    }
    if (sequence.empty()) {
      GPB anchor;
      anchor.id = next_id++;
      anchor.origin_block = &basic_block;
      sequence.push_back(anchor.id);
      graph.addBlock(std::move(anchor));
    }
    first[&basic_block] = sequence.front();
    last[&basic_block] = sequence.back();
    for (std::size_t i = 1; i < sequence.size(); ++i)
      graph.addEdge(sequence[i - 1], sequence[i]);
  }

  GPB end;
  end.id = next_id++;
  end.kind = GPBKind::End;
  graph.addBlock(end);
  graph.setExit(end.id);

  if (!function.empty())
    graph.addEdge(graph.entry(), first[&function.getEntryBlock()]);
  for (llvm::BasicBlock &basic_block : function) {
    if (entry_reachable_blocks_.count(&basic_block) == 0)
      continue;
    if (llvm::succ_empty(&basic_block)) {
      graph.addEdge(last[&basic_block], graph.exit());
      continue;
    }
    for (llvm::BasicBlock *successor : llvm::successors(&basic_block))
      graph.addEdge(last[&basic_block], first[successor]);
  }

  GPUSet support;
  for (auto &[id, block] : graph.blocks()) {
    (void)id;
    for (auto iterator = block.gpus.begin(); iterator != block.gpus.end();) {
      if (iterator->flow_insensitive) {
        support.insert(*iterator);
        iterator = block.gpus.erase(iterator);
      } else {
        ++iterator;
      }
    }
    for (auto iterator = block.original_gpus.begin();
         iterator != block.original_gpus.end();) {
      if (iterator->flow_insensitive)
        iterator = block.original_gpus.erase(iterator);
      else
        ++iterator;
    }
  }
  // LLVM SSA definitions are immutable and therefore available as support
  // facts independently of CFG order. Keep those facts indexed by their SSA
  // source instead of eagerly reducing every fact against the whole support
  // relation. Consumers already reduce transitively through supportGPUs();
  // eager all-to-all normalization duplicates that work and is exponential on
  // phi/select-heavy code.
  graph.setSupportGPUs(std::move(support));
  graph.eliminateEmptyGPBs();
  addBoundaryDefinitions(graph, function);
  return graph;
}

void LLVMFrontend::addBoundaryDefinitions(
    GPG &graph, const llvm::Function &function) const {
  (void)function;
  GPUSet definitions;
  auto collect_from = [&](const GPUSet &gpus) {
    for (const GPU &gpu : gpus) {
      for (const Access *access : {&gpu.source, &gpu.target}) {
        const MemoryLocation *location = model_.getLocation(access->location);
        if (!location || access->indirections.empty())
          continue;
        const bool live_on_entry = location->kind == LocationKind::Global ||
                                   location->kind == LocationKind::Formal ||
                                   location->kind == LocationKind::Heap ||
                                   location->kind == LocationKind::Unknown ||
                                   (location->kind == LocationKind::Stack &&
                                    location->address_escaped);
        if (!live_on_entry)
          continue;

        std::vector<Indirection> prefix;
        for (const Indirection &step : access->indirections.elements()) {
          prefix.push_back(step);
          Access boundary_source =
              model_.access(access->location, IndirectionList(prefix));
          definitions.insert(GPG::makeBoundaryDefinition(boundary_source));
        }
      }
    }
  };
  collect_from(graph.supportGPUs());
  for (const auto &[id, block] : graph.blocks()) {
    (void)id;
    collect_from(block.gpus);
  }
  graph.setBoundaryDefinitions(std::move(definitions));
}

void LLVMFrontend::collectGlobalInitializers() {
  for (llvm::GlobalVariable &global : model_.module().globals()) {
    if (!global.hasInitializer())
      continue;
    Access base = model_.access(model_.objectLocation(&global), {});
    emitInitializer(*global.getInitializer(), base, {});
  }
}

void LLVMFrontend::emitInitializer(const llvm::Constant &constant,
                                   const Access &base,
                                   std::vector<Indirection> path) {
  if (llvm::isa<llvm::ConstantAggregateZero>(constant)) {
    emitZeroInitializer(*constant.getType(), base, std::move(path));
    return;
  }
  if (constant.getType()->isPointerTy()) {
    path.push_back(Indirection::dereference());
    Access source = append(base, path);
    for (const Access &target : readAccesses(&constant)) {
      GPU update;
      update.source = source;
      update.target = target;
      update.kind = GPUKind::Assignment;
      update.source_may_alias_multiple =
          model_.forcesWeakUpdate(source.location) ||
          std::any_of(source.indirections.elements().begin(),
                      source.indirections.elements().end(),
                      [](const Indirection &step) {
                        return step.kind == IndirectionKind::AnyField;
                      });
      global_initializers_.insert(std::move(update));
    }
    return;
  }

  if (!isAggregate(constant.getType()))
    return;
  for (unsigned index = 0; index < constant.getNumOperands(); ++index) {
    const auto *element =
        llvm::dyn_cast<llvm::Constant>(constant.getOperand(index));
    if (!element)
      continue;
    std::vector<Indirection> element_path = path;
    if (constant.getType()->isStructTy() && config_.field_sensitive) {
      element_path.push_back(Indirection::fieldAt(index));
    } else if (config_.array_index_sensitive) {
      element_path.push_back(Indirection::fieldAt(index));
    } else {
      element_path.push_back(Indirection::anyField());
    }
    emitInitializer(*element, base, std::move(element_path));
  }
}

void LLVMFrontend::emitZeroInitializer(const llvm::Type &type,
                                       const Access &base,
                                       std::vector<Indirection> path) {
  if (type.isPointerTy()) {
    path.push_back(Indirection::dereference());
    GPU update;
    update.source = append(base, path);
    update.target = model_.access(model_.nullLocation(), {});
    update.kind = GPUKind::Assignment;
    update.source_may_alias_multiple =
        model_.forcesWeakUpdate(update.source.location);
    global_initializers_.insert(std::move(update));
    return;
  }

  if (const auto *structure = llvm::dyn_cast<llvm::StructType>(&type)) {
    for (unsigned index = 0; index < structure->getNumElements(); ++index) {
      std::vector<Indirection> child = path;
      child.push_back(config_.field_sensitive ? Indirection::fieldAt(index)
                                              : Indirection::anyField());
      emitZeroInitializer(*structure->getElementType(index), base,
                          std::move(child));
    }
    return;
  }
  if (const auto *array = llvm::dyn_cast<llvm::ArrayType>(&type)) {
    const std::uint64_t count =
        config_.array_index_sensitive
            ? array->getNumElements()
            : std::min<std::uint64_t>(1, array->getNumElements());
    for (std::uint64_t index = 0; index < count; ++index) {
      std::vector<Indirection> child = path;
      child.push_back(
          config_.array_index_sensitive
              ? Indirection::fieldAt(static_cast<std::int64_t>(index))
              : Indirection::anyField());
      emitZeroInitializer(*array->getElementType(), base, std::move(child));
    }
  }
}

} // namespace lotus::gpg
