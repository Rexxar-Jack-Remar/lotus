#include "IR/PDG/Analysis/LifetimeQuery.h"

#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Operator.h"

#include "IR/PDG/Analysis/LibraryModels.h"
#include "IR/PDG/Analysis/ValueFacts.h"

#include <deque>
#include <map>
#include <set>
#include <sstream>
#include <utility>

using namespace llvm;

namespace pdg {
namespace {

enum class Family { Memory, File, Descriptor };
enum class Identity { None, Base, Interior };

struct Object {
  const Value *origin;
  const Instruction *acquire;
  Family family;
};

struct Slot {
  const AllocaInst *base = nullptr;
  int64_t offset = 0;
  bool operator<(const Slot &other) const {
    if (base != other.base)
      return std::less<const AllocaInst *>()(base, other.base);
    return offset < other.offset;
  }
};

struct Comparison {
  const Value *left;
  const Value *right;
  ICmpInst::Predicate predicate;
  bool operator<(const Comparison &other) const {
    if (left != other.left)
      return std::less<const Value *>()(left, other.left);
    if (right != other.right)
      return std::less<const Value *>()(right, other.right);
    return predicate < other.predicate;
  }
};

struct State {
  bool acquired = false;
  const Instruction *released = nullptr;
  bool escaped = false;
  std::map<const Value *, Identity> aliases;
  std::map<Slot, Identity> slots;
  std::map<const Value *, const Value *> scalar_roots;
  std::map<Slot, const Value *> scalar_slots;
  std::map<const Value *, bool> conditions;
  std::map<const Value *, const ConstantInt *> integer_equal;
  std::map<const Value *, std::set<const ConstantInt *>> integer_excluded;
  std::map<Comparison, bool> comparisons;
  std::set<const AllocaInst *> escaped_locals;
};

struct Work {
  const BasicBlock *block;
  const BasicBlock *predecessor;
  State state;
};

bool named(const Function &function, StringRef name) {
  return function.isDeclaration() &&
         ValueFacts::hasLibraryName(function, name.str(), true);
}

Optional<Family> acquireFamily(const CallBase &call) {
  const Function *callee = ValueFacts::callee(call);
  if (!callee || !callee->isDeclaration())
    return None;
  // realloc has success-dependent effects on both old and new objects, and is
  // deliberately an ownership barrier until a relational model is available.
  if (LibraryModels::allocation(*callee) != AllocationKind::Unknown &&
      !named(*callee, "realloc"))
    return Family::Memory;
  for (StringRef name :
       {"fopen", "fopen64", "fdopen", "tmpfile", "tmpfile64", "popen"})
    if (named(*callee, name))
      return Family::File;
  for (StringRef name : {"open", "open64", "creat", "creat64", "socket",
                         "accept", "accept4", "dup"})
    if (named(*callee, name))
      return Family::Descriptor;
  return None;
}

bool releases(const CallBase &call, Family family) {
  const Function *callee = ValueFacts::callee(call);
  if (!callee || !callee->isDeclaration() || call.arg_empty())
    return false;
  if (family == Family::Memory)
    return LibraryModels::release(*callee) != ReleaseKind::Unknown;
  if (family == Family::File)
    return named(*callee, "fclose") || named(*callee, "pclose");
  return named(*callee, "close");
}

/// These slots are exact addresses, rather than may-alias root objects.
Optional<Slot> localSlot(const Value *pointer, const DataLayout &layout) {
  if (!pointer || !pointer->getType()->isPointerTy())
    return None;
  int64_t offset = 0;
  const Value *base = GetPointerBaseWithConstantOffset(pointer, offset, layout);
  const auto *allocation = dyn_cast<AllocaInst>(base);
  if (!allocation)
    return None;
  return Slot{allocation, offset};
}

Identity identity(const Value *value, const State &state) {
  if (!value)
    return Identity::None;
  auto found = state.aliases.find(value);
  if (found != state.aliases.end())
    return found->second;
  if (const auto *cast = dyn_cast<Operator>(value)) {
    if (cast->getOpcode() == Instruction::BitCast ||
        cast->getOpcode() == Instruction::AddrSpaceCast)
      return identity(cast->getOperand(0), state);
    if (cast->getOpcode() == Instruction::GetElementPtr) {
      Identity base = identity(cast->getOperand(0), state);
      if (base == Identity::None)
        return base;
      const auto *gep = llvm::cast<GEPOperator>(cast);
      return gep->hasAllZeroIndices() ? base : Identity::Interior;
    }
  }
  return Identity::None;
}

void bind(State &state, const Value *value, Identity id) {
  if (id == Identity::None)
    state.aliases.erase(value);
  else
    state.aliases[value] = id;
}

/// An address passed to an unknown call can access any member of its local
/// aggregate. Byte-overlapping stores also invalidate aliases, even when the
/// offsets differ (for example union or casted stores).
bool invalidateSlots(State &state, const Slot &location,
                     const DataLayout &layout, uint64_t bytes = 0) {
  bool removed_alias = false;
  std::vector<Slot> invalidated;
  for (const auto &entry : state.scalar_slots) {
    if (entry.first.base != location.base)
      continue;
    Type *type = entry.second->getType();
    uint64_t width =
        type->isSized() ? layout.getTypeStoreSize(type).getKnownMinValue() : 0;
    if (bytes == 0 || width == 0 ||
        (entry.first.offset < location.offset + static_cast<int64_t>(bytes) &&
         location.offset < entry.first.offset + static_cast<int64_t>(width)))
      invalidated.push_back(entry.first);
  }
  for (const Slot &slot : invalidated) {
    removed_alias |= state.slots.erase(slot) != 0;
    state.scalar_slots.erase(slot);
  }
  return removed_alias;
}

// Recognize Boolean identity/inversion. A later test of the same SSA condition
// cannot select the opposite alternative. Conditions with unrelated arithmetic
// remain unknown and are explicitly outside the feasibility guarantee.
const Value *scalarRoot(const Value *value, const State &state) {
  auto found = state.scalar_roots.find(value);
  return found == state.scalar_roots.end() ? value : found->second;
}

Optional<Slot> localSlot(const Value *pointer, const DataLayout &layout,
                         const State &state) {
  if (!pointer || !pointer->getType()->isPointerTy())
    return None;
  int64_t offset = 0;
  const Value *base = GetPointerBaseWithConstantOffset(pointer, offset, layout);
  base = scalarRoot(base, state);
  auto slot = localSlot(base, layout);
  if (slot)
    slot->offset += offset;
  return slot;
}

const AllocaInst *localBase(const Value *pointer, const State &state) {
  if (!pointer || !pointer->getType()->isPointerTy())
    return nullptr;
  const Value *base = scalarRoot(getUnderlyingObject(pointer), state);
  return dyn_cast<AllocaInst>(getUnderlyingObject(base));
}

std::pair<Comparison, bool> comparison(const ICmpInst &compare,
                                       const State &state) {
  const Value *left = scalarRoot(compare.getOperand(0), state);
  const Value *right = scalarRoot(compare.getOperand(1), state);
  auto predicate = compare.getPredicate();
  if (std::less<const Value *>()(right, left)) {
    std::swap(left, right);
    predicate = ICmpInst::getSwappedPredicate(predicate);
  }
  auto inverse = ICmpInst::getInversePredicate(predicate);
  bool invert = inverse < predicate;
  return {{left, right, invert ? inverse : predicate}, invert};
}

std::pair<const Value *, bool> conditionRoot(const Value *value,
                                             const State &state) {
  bool invert = false;
  for (unsigned depth = 0; depth < 32; ++depth) {
    value = scalarRoot(value, state);
    if (const auto *binary = dyn_cast<BinaryOperator>(value)) {
      if (binary->getOpcode() == Instruction::Xor &&
          binary->getType()->isIntegerTy(1)) {
        const auto *constant = dyn_cast<ConstantInt>(binary->getOperand(1));
        if (constant && constant->isOne()) {
          invert = !invert;
          value = binary->getOperand(0);
          continue;
        }
      }
    }
    if (const auto *compare = dyn_cast<ICmpInst>(value)) {
      const Value *tested = compare->getOperand(0);
      const Value *other = compare->getOperand(1);
      if (isa<Constant>(tested))
        std::swap(tested, other);
      const auto *constant = dyn_cast<Constant>(other);
      if (constant && constant->isNullValue() &&
          (compare->getPredicate() == ICmpInst::ICMP_EQ ||
           compare->getPredicate() == ICmpInst::ICMP_NE)) {
        invert ^= compare->getPredicate() == ICmpInst::ICMP_EQ;
        value = tested;
        continue;
      }
    }
    break;
  }
  return {value, invert};
}

Optional<bool> condition(const Value *value, const State &state,
                         Family family) {
  if (const auto *constant = dyn_cast<ConstantInt>(value))
    return !constant->isZero();
  auto root = conditionRoot(value, state);
  if (const auto *constant = dyn_cast<ConstantInt>(root.first))
    return (!constant->isZero()) != root.second;
  auto found = state.conditions.find(root.first);
  if (found != state.conditions.end())
    return found->second != root.second;
  auto integer = state.integer_equal.find(root.first);
  if (integer != state.integer_equal.end())
    return (!integer->second->isZero()) != root.second;
  if (const auto *compare = dyn_cast<ICmpInst>(root.first)) {
    auto normalized = comparison(*compare, state);
    auto known = state.comparisons.find(normalized.first);
    if (known != state.comparisons.end())
      return known->second != (normalized.second != root.second);
  }
  const auto *compare = dyn_cast<ICmpInst>(value);
  if (!compare)
    return None;
  if (compare->getPredicate() == ICmpInst::ICMP_EQ ||
      compare->getPredicate() == ICmpInst::ICMP_NE) {
    const Value *tested = scalarRoot(compare->getOperand(0), state);
    const auto *constant = dyn_cast<ConstantInt>(compare->getOperand(1));
    if (!constant) {
      tested = scalarRoot(compare->getOperand(1), state);
      constant = dyn_cast<ConstantInt>(compare->getOperand(0));
    }
    if (constant) {
      auto equal = state.integer_equal.find(tested);
      if (equal != state.integer_equal.end())
        return (equal->second == constant) ==
               (compare->getPredicate() == ICmpInst::ICMP_EQ);
      auto excluded = state.integer_excluded.find(tested);
      if (excluded != state.integer_excluded.end() &&
          excluded->second.count(constant))
        return compare->getPredicate() == ICmpInst::ICMP_NE;
    }
  }
  if (!state.acquired)
    return None;
  const Value *tracked = compare->getOperand(0);
  const Value *constant = compare->getOperand(1);
  ICmpInst::Predicate predicate = compare->getPredicate();
  if (identity(tracked, state) == Identity::None) {
    std::swap(tracked, constant);
    predicate = ICmpInst::getSwappedPredicate(predicate);
  }
  if (identity(tracked, state) != Identity::Base)
    return None;
  if (family != Family::Descriptor && isa<ConstantPointerNull>(constant)) {
    if (predicate == ICmpInst::ICMP_EQ)
      return false;
    if (predicate == ICmpInst::ICMP_NE)
      return true;
  }
  if (family == Family::Descriptor) {
    const auto *integer = dyn_cast<ConstantInt>(constant);
    if (!integer)
      return None;
    if (integer->isMinusOne()) {
      if (predicate == ICmpInst::ICMP_EQ || predicate == ICmpInst::ICMP_SLE)
        return false;
      if (predicate == ICmpInst::ICMP_NE || predicate == ICmpInst::ICMP_SGT)
        return true;
    }
    if (integer->isZero()) {
      if (predicate == ICmpInst::ICMP_SLT)
        return false;
      if (predicate == ICmpInst::ICMP_SGE)
        return true;
    }
  }
  return None;
}

bool assumeInteger(State &state, const Value *value,
                   const ConstantInt *constant, bool equal) {
  value = scalarRoot(value, state);
  if (const auto *known = dyn_cast<ConstantInt>(value))
    return (known == constant) == equal;
  auto known = state.integer_equal.find(value);
  if (known != state.integer_equal.end())
    return (known->second == constant) == equal;
  auto excluded = state.integer_excluded.find(value);
  if (equal && excluded != state.integer_excluded.end() &&
      excluded->second.count(constant))
    return false;
  auto boolean = state.conditions.find(value);
  if (equal && boolean != state.conditions.end() &&
      boolean->second == constant->isZero())
    return false;
  if (equal)
    state.integer_equal[value] = constant;
  else
    state.integer_excluded[value].insert(constant);
  return true;
}

bool assume(State &state, const Value *value, bool truth, Family family) {
  auto known = condition(value, state, family);
  if (known && *known != truth)
    return false;
  auto root = conditionRoot(value, state);
  state.conditions[root.first] = truth != root.second;
  if (const auto *compare = dyn_cast<ICmpInst>(root.first)) {
    auto normalized = comparison(*compare, state);
    bool normalized_truth = truth != (root.second != normalized.second);
    auto known = state.comparisons.find(normalized.first);
    if (known != state.comparisons.end() && known->second != normalized_truth)
      return false;
    state.comparisons[normalized.first] = normalized_truth;
  }
  if (const auto *compare = dyn_cast<ICmpInst>(value)) {
    if (compare->getPredicate() == ICmpInst::ICMP_EQ ||
        compare->getPredicate() == ICmpInst::ICMP_NE) {
      const Value *tested = compare->getOperand(0);
      const auto *constant = dyn_cast<ConstantInt>(compare->getOperand(1));
      if (!constant) {
        tested = compare->getOperand(1);
        constant = dyn_cast<ConstantInt>(compare->getOperand(0));
      }
      if (constant && !assumeInteger(state, tested, constant,
                                     truth == (compare->getPredicate() ==
                                               ICmpInst::ICMP_EQ)))
        return false;
    }
  }
  return true;
}

std::string key(const Work &work) {
  std::ostringstream out;
  out << work.block << ':' << work.predecessor << ':' << work.state.acquired
      << ':' << work.state.released << ':' << work.state.escaped;
  for (const auto &alias : work.state.aliases)
    out << 'a' << alias.first << ':' << static_cast<int>(alias.second);
  for (const auto &slot : work.state.slots)
    out << 's' << slot.first.base << ':' << slot.first.offset << ':'
        << static_cast<int>(slot.second);
  for (const auto &entry : work.state.scalar_roots)
    out << 'r' << entry.first << ':' << entry.second;
  for (const auto &entry : work.state.scalar_slots)
    out << 'v' << entry.first.base << ':' << entry.first.offset << ':'
        << entry.second;
  for (const auto &entry : work.state.conditions)
    out << 'c' << entry.first << ':' << entry.second;
  for (const auto &entry : work.state.integer_equal)
    out << 'e' << entry.first << ':' << entry.second;
  for (const auto &entry : work.state.integer_excluded) {
    out << 'n' << entry.first;
    for (const auto *constant : entry.second)
      out << ':' << constant;
  }
  for (const auto &entry : work.state.comparisons)
    out << 'p' << entry.first.left << ':' << entry.first.right << ':'
        << entry.first.predicate << ':' << entry.second;
  for (const AllocaInst *local : work.state.escaped_locals)
    out << 'l' << local;
  return out.str();
}

bool dereferences(const CallBase &call, unsigned argument) {
  if (const auto *memory = dyn_cast<MemIntrinsic>(&call)) {
    if (const auto *length = dyn_cast<ConstantInt>(memory->getLength()))
      if (length->isZero())
        return false;
    return argument == 0 || (isa<MemTransferInst>(memory) && argument == 1);
  }
  const Function *callee = ValueFacts::callee(call);
  if (!callee || !callee->isDeclaration())
    return false;
  if (LibraryModels::readsOnly(*callee))
    return argument <
           (named(*callee, "strlen") || named(*callee, "wcslen") ? 1u : 2u);
  for (StringRef name : {"memcpy", "memmove", "memcmp", "strcpy", "strncpy",
                         "strcat", "strncat", "strcmp", "strncmp"})
    if (named(*callee, name)) {
      if (call.arg_size() > 2)
        if (const auto *length = dyn_cast<ConstantInt>(call.getArgOperand(2)))
          if (length->isZero())
            return false;
      return argument < 2;
    }
  if (named(*callee, "memset")) {
    if (call.arg_size() > 2)
      if (const auto *length = dyn_cast<ConstantInt>(call.getArgOperand(2)))
        if (length->isZero())
          return false;
    return argument == 0;
  }
  for (StringRef name : {"puts", "fputs", "sprintf"})
    if (named(*callee, name))
      return argument == 0;
  if (named(*callee, "fgets") || named(*callee, "snprintf")) {
    if (call.arg_size() > 1)
      if (const auto *length = dyn_cast<ConstantInt>(call.getArgOperand(1)))
        if (length->isZero() ||
            (named(*callee, "fgets") && length->getValue().isNegative()))
          return false;
    return argument == 0;
  }
  if (named(*callee, "read") || named(*callee, "write")) {
    if (call.arg_size() > 2)
      if (const auto *length = dyn_cast<ConstantInt>(call.getArgOperand(2)))
        if (length->isZero())
          return false;
    return argument == 1;
  }
  return false;
}

bool harmlessCall(const CallBase &call) {
  if (isa<DbgInfoIntrinsic>(call))
    return true;
  if (const auto *intrinsic = dyn_cast<IntrinsicInst>(&call))
    return intrinsic->getIntrinsicID() == Intrinsic::lifetime_start ||
           intrinsic->getIntrinsicID() == Intrinsic::lifetime_end;
  return false;
}

struct Explorer {
  const Function &function;
  const DataLayout &layout;
  const Object &object;
  size_t limit;
  LifetimeQueryResult &result;
  bool any_release = false;
  bool any_escape = false;
  bool incomplete = false;
  bool budget_limited = false;
  std::set<const Instruction *> leaking_exits;
  std::set<std::pair<std::string, const Instruction *>> emitted;

  Explorer(const Function &analyzed_function, const DataLayout &data_layout,
           const Object &tracked_object, size_t state_budget,
           LifetimeQueryResult &output)
      : function(analyzed_function), layout(data_layout),
        object(tracked_object), limit(state_budget), result(output) {}

  void finding(StringRef id, StringRef message, const Instruction *site,
               const Instruction *release = nullptr) {
    if (!emitted.insert({id.str(), site}).second)
      return;
    LifetimeFinding item{id.str(), message.str(), site, {}};
    if (object.acquire)
      item.evidence.push_back(object.acquire);
    if (release)
      item.evidence.push_back(release);
    result.findings.push_back(std::move(item));
  }

  void use(const Value *value, const Instruction &instruction, State &state) {
    if (object.family == Family::Memory && state.released && !state.escaped &&
        identity(value, state) != Identity::None)
      finding("cpp/use-after-free", "Memory is accessed after its release.",
              &instruction, state.released);
  }

  void escape(State &state) {
    if (!state.acquired)
      return;
    state.escaped = true;
    any_escape = true;
  }

  /// Address escape is a property of the local location, even before the
  /// tracked heap allocation exists. A later store into a published slot can
  /// expose that allocation to an unknown callback. Follow exact local
  /// pointer-to-pointer slots while the address provenance remains known.
  void escapeLocal(const AllocaInst *local, State &state) {
    if (!state.escaped_locals.insert(local).second)
      return;
    for (const auto &entry : state.scalar_slots) {
      if (entry.first.base != local)
        continue;
      if (state.slots.count(entry.first))
        escape(state);
      if (const AllocaInst *nested = localBase(entry.second, state))
        escapeLocal(nested, state);
    }
  }

  bool instruction(const Instruction &instruction, State &state) {
    if (&instruction == object.acquire) {
      if (state.acquired) {
        // A repeated static acquire creates another dynamic object; collapsing
        // these objects would manufacture stale aliases and double frees.
        incomplete = true;
        return false;
      }
      state.acquired = true;
      bind(state, &instruction, Identity::Base);
      return true;
    }
    if (const auto *load = dyn_cast<LoadInst>(&instruction)) {
      use(load->getPointerOperand(), instruction, state);
      auto slot = localSlot(load->getPointerOperand(), layout, state);
      Identity id = Identity::None;
      if (slot) {
        auto found = state.slots.find(*slot);
        if (found != state.slots.end())
          id = found->second;
        auto scalar = state.scalar_slots.find(*slot);
        if (scalar != state.scalar_slots.end())
          state.scalar_roots[load] = scalar->second;
        else
          state.scalar_roots.erase(load);
      }
      bind(state, load, id);
    } else if (const auto *store = dyn_cast<StoreInst>(&instruction)) {
      use(store->getPointerOperand(), instruction, state);
      Identity id = identity(store->getValueOperand(), state);
      auto slot = localSlot(store->getPointerOperand(), layout, state);
      if (slot) {
        invalidateSlots(
            state, *slot, layout,
            layout.getTypeStoreSize(store->getValueOperand()->getType())
                .getKnownMinValue());
        state.scalar_slots[*slot] = scalarRoot(store->getValueOperand(), state);
        if (id == Identity::None)
          state.slots.erase(*slot);
        else
          state.slots[*slot] = id;
        if (state.escaped_locals.count(slot->base)) {
          if (id != Identity::None)
            escape(state);
          if (const AllocaInst *stored_address =
                  localBase(store->getValueOperand(), state))
            escapeLocal(stored_address, state);
        }
      } else if (id != Identity::None) {
        escape(state);
      } else {
        if (const AllocaInst *stored_address =
                localBase(store->getValueOperand(), state))
          escapeLocal(stored_address, state);
        // An unknown local address can overlap one of the precise slots.
        // Invalidate their aliases rather than preserving a stale handle.
        const Value *base = store->getPointerOperand()->stripPointerCasts();
        if (const auto *gep = dyn_cast<GEPOperator>(base))
          base = gep->getPointerOperand()->stripPointerCasts();
        if (const auto *alloca = dyn_cast<AllocaInst>(base))
          if (invalidateSlots(state, Slot{alloca, 0}, layout))
            escape(state);
      }
    } else if (const auto *atomic = dyn_cast<AtomicRMWInst>(&instruction)) {
      use(atomic->getPointerOperand(), instruction, state);
      if (identity(atomic->getValOperand(), state) != Identity::None)
        escape(state);
      if (auto slot = localSlot(atomic->getPointerOperand(), layout, state))
        state.slots.erase(*slot);
      if (auto slot = localSlot(atomic->getPointerOperand(), layout, state))
        state.scalar_slots.erase(*slot);
    } else if (const auto *atomic = dyn_cast<AtomicCmpXchgInst>(&instruction)) {
      use(atomic->getPointerOperand(), instruction, state);
      if (identity(atomic->getNewValOperand(), state) != Identity::None)
        escape(state);
      if (auto slot = localSlot(atomic->getPointerOperand(), layout, state))
        state.slots.erase(*slot);
      if (auto slot = localSlot(atomic->getPointerOperand(), layout, state))
        state.scalar_slots.erase(*slot);
    } else if (const auto *select = dyn_cast<SelectInst>(&instruction)) {
      auto known = condition(select->getCondition(), state, object.family);
      Identity left = identity(select->getTrueValue(), state);
      Identity right = identity(select->getFalseValue(), state);
      bind(state, select,
           known           ? (*known ? left : right)
           : left == right ? left
                           : Identity::None);
      // Different alternatives require a disjunction to prove ownership.
      // Prevent an unsound leak conclusion if the handle may be selected.
      if (!known && left != right &&
          (left != Identity::None || right != Identity::None))
        escape(state);
      if (!known)
        for (const Value *value :
             {select->getTrueValue(), select->getFalseValue()})
          if (auto slot = localSlot(value, layout, state))
            if (state.slots.count(*slot))
              escape(state);
    } else if (const auto *call = dyn_cast<CallBase>(&instruction)) {
      if (harmlessCall(*call))
        return true;
      if (releases(*call, object.family) &&
          identity(call->getArgOperand(0), state) == Identity::Base) {
        if (!state.escaped) {
          if (state.released && object.family == Family::Memory)
            finding("cpp/double-free", "The same memory is released twice.",
                    call, state.released);
          any_release = true;
          state.released = call;
        }
      } else {
        for (unsigned index = 0; index < call->arg_size(); ++index) {
          const Value *argument = call->getArgOperand(index);
          if (identity(argument, state) != Identity::None) {
            if (dereferences(*call, index))
              use(argument, instruction, state);
            else
              escape(state);
          }
          // Passing the local address of a tracked slot can transfer ownership
          // or overwrite it through a pointer-to-pointer parameter.
          if (const AllocaInst *local = localBase(argument, state)) {
            escapeLocal(local, state);
            if (invalidateSlots(state, Slot{local, 0}, layout))
              escape(state);
          }
        }
      }
      if (call->doesNotReturn())
        return false;
    } else if (const auto *ret = dyn_cast<ReturnInst>(&instruction)) {
      if (const AllocaInst *address = localBase(ret->getReturnValue(), state))
        escapeLocal(address, state);
      if (identity(ret->getReturnValue(), state) != Identity::None)
        escape(state);
      if (state.acquired && !state.released && !state.escaped)
        leaking_exits.insert(ret);
    } else if (const auto *cast = dyn_cast<CastInst>(&instruction)) {
      if (cast->getOpcode() == Instruction::BitCast ||
          cast->getOpcode() == Instruction::AddrSpaceCast)
        bind(state, cast, identity(cast->getOperand(0), state));
      else if (identity(cast->getOperand(0), state) != Identity::None)
        // ptr/int roundtrips and descriptor width changes need an explicit
        // representation model. They cannot retain a guessed object identity.
        escape(state);
      else if (const AllocaInst *address =
                   localBase(cast->getOperand(0), state))
        escapeLocal(address, state);
    } else if (const auto *gep = dyn_cast<GetElementPtrInst>(&instruction)) {
      state.aliases.erase(gep);
      bind(state, gep, identity(gep, state));
    }
    return true;
  }

  void run() {
    State initial;
    if (!object.acquire) {
      initial.acquired = true;
      bind(initial, object.origin, Identity::Base);
    }
    std::deque<Work> queue;
    queue.push_back({&function.getEntryBlock(), nullptr, std::move(initial)});
    std::set<std::string> seen;
    while (!queue.empty()) {
      Work work = std::move(queue.front());
      queue.pop_front();
      if (!seen.insert(key(work)).second)
        continue;
      if (limit != 0 && seen.size() > limit) {
        incomplete = true;
        budget_limited = true;
        break;
      }
      State &state = work.state;
      // LLVM phi operands are selected simultaneously on the incoming edge.
      State incoming = state;
      for (const PHINode &phi : work.block->phis()) {
        int index =
            work.predecessor ? phi.getBasicBlockIndex(work.predecessor) : -1;
        bind(state, &phi,
             index < 0 ? Identity::None
                       : identity(phi.getIncomingValue(index), incoming));
        if (index >= 0)
          state.scalar_roots[&phi] =
              scalarRoot(phi.getIncomingValue(index), incoming);
      }
      bool alive = true;
      for (const Instruction &item : *work.block) {
        // An instruction on a back edge denotes a fresh dynamic value.
        state.conditions.erase(&item);
        state.integer_equal.erase(&item);
        state.integer_excluded.erase(&item);
        for (auto it = state.comparisons.begin();
             it != state.comparisons.end();)
          if (it->first.left == &item || it->first.right == &item)
            it = state.comparisons.erase(it);
          else
            ++it;
        if (!isa<PHINode>(item) && !instruction(item, state)) {
          alive = false;
          break;
        }
      }
      if (!alive)
        continue;
      const Instruction *terminator = work.block->getTerminator();
      if (const auto *branch = dyn_cast<BranchInst>(terminator)) {
        if (!branch->isConditional()) {
          queue.push_back({branch->getSuccessor(0), work.block, state});
        } else {
          for (unsigned index = 0; index < 2; ++index) {
            State alternative = state;
            if (assume(alternative, branch->getCondition(), index == 0,
                       object.family))
              queue.push_back({branch->getSuccessor(index), work.block,
                               std::move(alternative)});
          }
        }
      } else if (const auto *dispatch = dyn_cast<SwitchInst>(terminator)) {
        State fallback = state;
        bool default_possible = true;
        for (const auto &entry : dispatch->cases()) {
          State alternative = state;
          if (assumeInteger(alternative, dispatch->getCondition(),
                            entry.getCaseValue(), true))
            queue.push_back(
                {entry.getCaseSuccessor(), work.block, std::move(alternative)});
          default_possible &= assumeInteger(fallback, dispatch->getCondition(),
                                            entry.getCaseValue(), false);
        }
        if (default_possible)
          queue.push_back(
              {dispatch->getDefaultDest(), work.block, std::move(fallback)});
      } else {
        for (const BasicBlock *successor : successors(work.block))
          queue.push_back({successor, work.block, state});
      }
    }
    if (incomplete) {
      ++result.incomplete_objects;
      if (budget_limited) {
        ++result.budget_limited_objects;
        result.state_limit_hit = true;
      }
      return;
    }
    if (!object.acquire || leaking_exits.empty())
      return;
    StringRef never_id =
        object.family == Family::Memory ? "cpp/memory-never-freed"
        : object.family == Family::File ? "cpp/file-never-closed"
                                        : "cpp/descriptor-never-closed";
    if (!any_release && !any_escape) {
      finding(never_id, "This resource is never released before returning.",
              object.acquire);
      result.findings.back().evidence.push_back(*leaking_exits.begin());
    } else if ((any_release || any_escape) &&
               object.family != Family::Descriptor) {
      StringRef may_id = object.family == Family::Memory
                             ? "cpp/memory-may-not-be-freed"
                             : "cpp/file-may-not-be-closed";
      finding(may_id, "This resource is not released on a returning CFG path.",
              object.acquire);
      result.findings.back().evidence.push_back(*leaking_exits.begin());
    }
  }
};

} // namespace

const std::vector<LifetimeRuleDescriptor> &LifetimeQuery::catalog() {
  static const std::string coverage =
      "Intraprocedural CFG lifecycle with SSA/phi and exact local stack "
      "aliases; "
      "known C allocation/release APIs and heap dereferences; unknown "
      "ownership "
      "effects escape. Preserves Boolean alternatives and allocation-failure "
      "guards and integer equality/switch alternatives; general relational "
      "feasibility, realloc, indirect calls, C++ "
      "destructors, heap-field aliases and interprocedural ownership are "
      "excluded.";
  static const std::vector<LifetimeRuleDescriptor> rules = {
      {"cpp/double-free", "warning", "Critical/DoubleFree.ql", coverage},
      {"cpp/use-after-free", "warning", "Critical/UseAfterFree.ql", coverage},
      {"cpp/memory-never-freed", "warning", "Critical/MemoryNeverFreed.ql",
       coverage},
      {"cpp/memory-may-not-be-freed", "warning",
       "Critical/MemoryMayNotBeFreed.ql", coverage},
      {"cpp/file-never-closed", "warning", "Critical/FileNeverClosed.ql",
       coverage},
      {"cpp/file-may-not-be-closed", "warning",
       "Critical/FileMayNotBeClosed.ql", coverage},
      {"cpp/descriptor-never-closed", "warning",
       "Critical/DescriptorNeverClosed.ql", coverage}};
  return rules;
}

LifetimeQueryResult LifetimeQuery::analyze(const Module &module,
                                           size_t max_states_per_object) const {
  LifetimeQueryResult result;
  for (const Function &function : module) {
    if (function.isDeclaration())
      continue;
    std::vector<Object> objects;
    bool has_memory_release = false;
    for (const BasicBlock &block : function)
      for (const Instruction &instruction : block) {
        if (const auto *call = dyn_cast<CallBase>(&instruction))
          has_memory_release |= releases(*call, Family::Memory);
        // Invoke acquisition needs separate normal/unwind return states.
        if (const auto *call = dyn_cast<CallInst>(&instruction))
          if (auto family = acquireFamily(*call))
            objects.push_back({call, call, *family});
      }
    // External parameters matter for flow-after-free rules, but never for
    // acquisition leaks. Avoid exploring every ordinary pointer argument.
    if (has_memory_release)
      for (const Argument &argument : function.args())
        if (argument.getType()->isPointerTy())
          objects.push_back({&argument, nullptr, Family::Memory});
    for (const Object &object : objects) {
      Explorer explorer{function, module.getDataLayout(), object,
                        max_states_per_object, result};
      explorer.run();
    }
  }
  // Different origins can converge to the same reported source location. Keep
  // one finding per rule and LLVM site, retaining a concrete release witness.
  std::set<std::pair<std::string, const Instruction *>> seen;
  std::vector<LifetimeFinding> unique;
  for (LifetimeFinding &finding : result.findings)
    if (seen.insert({finding.rule_id, finding.site}).second)
      unique.push_back(std::move(finding));
  result.findings = std::move(unique);
  return result;
}

} // namespace pdg
