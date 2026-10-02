#include "IR/PDG/Analysis/StateQuery.h"

#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/IntrinsicInst.h"

#include "IR/PDG/Analysis/ValueFacts.h"

#include <algorithm>
#include <deque>
#include <set>

using namespace llvm;

namespace pdg {
namespace {

constexpr unsigned Null = 1, NonNull = 2, UnknownNull = 4;
constexpr unsigned Uninit = 1, Init = 2, UnknownInit = 4;

struct NullFact {
  unsigned bits = UnknownNull;
  // Identity relates a checked SSA pointer to copies still holding that value.
  // A strong store changes identity, so a check never validates a later store.
  const Value *identity = nullptr;
  std::set<const Instruction *> origins;
  bool operator==(const NullFact &other) const {
    return bits == other.bits && identity == other.identity &&
           origins == other.origins;
  }
};

struct Cell {
  unsigned initialization = UnknownInit;
  NullFact pointer;
  const Instruction *last_write = nullptr;
  bool escaped = false;
  const ConstantInt *integer = nullptr;
  bool operator==(const Cell &other) const {
    return initialization == other.initialization && pointer == other.pointer &&
           last_write == other.last_write && escaped == other.escaped &&
           integer == other.integer;
  }
};

struct State {
  bool reachable = false;
  std::map<const AllocaInst *, Cell> cells;
  std::map<const Value *, NullFact> pointers;
  std::map<const Value *, const ConstantInt *> integers;
  bool operator==(const State &other) const {
    return reachable == other.reachable && cells == other.cells &&
           pointers == other.pointers && integers == other.integers;
  }
};

using NullableFunctions = std::set<const Function *>;
using ParameterReads =
    std::map<const Function *, std::map<unsigned, const Instruction *>>;

NullFact mergeFact(const NullFact &a, const NullFact &b) {
  NullFact result = a;
  result.bits |= b.bits;
  if (a.identity != b.identity)
    result.identity = nullptr;
  result.origins.insert(b.origins.begin(), b.origins.end());
  return result;
}

void mergeState(State &to, const State &from) {
  if (!from.reachable)
    return;
  if (!to.reachable) {
    to = from;
    return;
  }
  for (auto &entry : to.cells) {
    auto found = from.cells.find(entry.first);
    Cell other = found == from.cells.end() ? Cell{} : found->second;
    entry.second.initialization |= other.initialization;
    entry.second.pointer = mergeFact(entry.second.pointer, other.pointer);
    entry.second.escaped |= other.escaped;
    if (entry.second.integer != other.integer)
      entry.second.integer = nullptr;
    if (entry.second.last_write != other.last_write)
      entry.second.last_write = nullptr;
  }
  for (const auto &entry : from.cells)
    if (!to.cells.count(entry.first)) {
      Cell cell = entry.second;
      cell.initialization |= UnknownInit;
      cell.pointer.bits |= UnknownNull;
      cell.last_write = nullptr;
      to.cells.emplace(entry.first, cell);
    }
  for (auto &entry : to.pointers) {
    auto found = from.pointers.find(entry.first);
    entry.second =
        mergeFact(entry.second,
                  found == from.pointers.end() ? NullFact{} : found->second);
  }
  for (const auto &entry : from.pointers)
    if (!to.pointers.count(entry.first))
      to.pointers.emplace(entry.first, mergeFact(NullFact{}, entry.second));
  for (auto &entry : to.integers) {
    auto found = from.integers.find(entry.first);
    if (found == from.integers.end() || found->second != entry.second)
      entry.second = nullptr;
  }
  for (const auto &entry : from.integers)
    if (!to.integers.count(entry.first))
      to.integers[entry.first] = nullptr;
}

// Only complete scalar cells are tracked: a byte store into an int is not a
// strong initialization of the int, and aggregate field initialization is a
// separate memory-range problem.
const AllocaInst *scalarCell(const Value *pointer, const DataLayout &layout,
                             const Type *access_type = nullptr) {
  if (!pointer || !pointer->getType()->isPointerTy())
    return nullptr;
  int64_t offset = 0;
  const Value *base = GetPointerBaseWithConstantOffset(pointer, offset, layout);
  const auto *alloca = dyn_cast<AllocaInst>(base);
  if (!alloca || offset != 0 || alloca->isArrayAllocation() ||
      !alloca->getAllocatedType()->isSingleValueType() ||
      !alloca->getAllocatedType()->isSized())
    return nullptr;
  if (access_type &&
      (!access_type->isSized() ||
       layout.getTypeStoreSize(const_cast<Type *>(access_type)) !=
           layout.getTypeAllocSize(alloca->getAllocatedType())))
    return nullptr;
  return alloca;
}

NullFact pointerFact(const Value *value, const State &state,
                     const NullableFunctions &nullable, unsigned depth = 0) {
  if (!value || !value->getType()->isPointerTy() || depth > 20)
    return {};
  auto known = state.pointers.find(value);
  if (known != state.pointers.end())
    return known->second;
  if (isa<ConstantPointerNull>(value))
    return {Null, value, {}};
  if (isa<AllocaInst>(value) || isa<GlobalValue>(value))
    return {NonNull, value, {}};
  if (const auto *call = dyn_cast<CallBase>(value)) {
    const Function *callee = ValueFacts::callee(*call);
    if (callee && nullable.count(callee))
      return {Null | NonNull, value, {call}};
    if (call->hasRetAttr(Attribute::NonNull))
      return {NonNull, value, {}};
    return {UnknownNull, value, {}};
  }
  if (const auto *gep = dyn_cast<GEPOperator>(value))
    return pointerFact(gep->getPointerOperand(), state, nullable, depth + 1);
  if (const auto *cast = dyn_cast<Operator>(value))
    if (cast->getOpcode() == Instruction::BitCast ||
        cast->getOpcode() == Instruction::AddrSpaceCast)
      return pointerFact(cast->getOperand(0), state, nullable, depth + 1);
  if (const auto *select = dyn_cast<SelectInst>(value)) {
    NullFact a =
        pointerFact(select->getTrueValue(), state, nullable, depth + 1);
    NullFact b =
        pointerFact(select->getFalseValue(), state, nullable, depth + 1);
    NullFact fact = mergeFact(a, b);
    fact.identity = value;
    return fact;
  }
  return {UnknownNull, value, {}};
}

const ConstantInt *integerFact(const Value *value, const State &state,
                               unsigned depth = 0) {
  if (!value || !value->getType()->isIntegerTy() || depth > 12)
    return nullptr;
  if (const auto *constant = dyn_cast<ConstantInt>(value))
    return constant;
  auto found = state.integers.find(value);
  if (found != state.integers.end())
    return found->second;
  if (const auto *cast = dyn_cast<CastInst>(value))
    if (cast->getOperand(0)->getType()->isIntegerTy())
      if (const ConstantInt *operand =
              integerFact(cast->getOperand(0), state, depth + 1))
        return dyn_cast<ConstantInt>(ConstantExpr::getCast(
            cast->getOpcode(), const_cast<ConstantInt *>(operand),
            cast->getType()));
  if (const auto *comparison = dyn_cast<ICmpInst>(value)) {
    const ConstantInt *a =
        integerFact(comparison->getOperand(0), state, depth + 1);
    const ConstantInt *b =
        integerFact(comparison->getOperand(1), state, depth + 1);
    if (a && b)
      return dyn_cast<ConstantInt>(ConstantExpr::getICmp(
          comparison->getPredicate(), const_cast<ConstantInt *>(a),
          const_cast<ConstantInt *>(b)));
  }
  if (const auto *binary = dyn_cast<BinaryOperator>(value)) {
    unsigned opcode = binary->getOpcode();
    if (opcode == Instruction::And || opcode == Instruction::Or ||
        opcode == Instruction::Xor || opcode == Instruction::Add ||
        opcode == Instruction::Sub || opcode == Instruction::Mul) {
      const ConstantInt *a =
          integerFact(binary->getOperand(0), state, depth + 1);
      const ConstantInt *b =
          integerFact(binary->getOperand(1), state, depth + 1);
      if (a && b)
        return dyn_cast<ConstantInt>(
            ConstantExpr::get(opcode, const_cast<ConstantInt *>(a),
                              const_cast<ConstantInt *>(b)));
    }
  }
  return nullptr;
}

bool isNullable(const NullFact &fact) {
  return (fact.bits & Null) && !(fact.bits & UnknownNull);
}

bool hasCallOrigin(const NullFact &fact) {
  return std::any_of(
      fact.origins.begin(), fact.origins.end(),
      [](const Instruction *origin) { return isa<CallBase>(origin); });
}

const Argument *parameterOrigin(const Value *pointer, const Function &function,
                                const DominatorTree &dominators) {
  const Value *root = getUnderlyingObject(pointer);
  if (const auto *argument = dyn_cast<Argument>(root))
    return argument->getParent() == &function ? argument : nullptr;
  // Recognize an unmodified frontend parameter spill, not a general pointer
  // load or a callee return. Every use must preserve the private spill slot.
  const auto *load = dyn_cast<LoadInst>(root);
  if (!load)
    return nullptr;
  const auto *slot =
      dyn_cast<AllocaInst>(load->getPointerOperand()->stripPointerCasts());
  if (!slot || !slot->getAllocatedType()->isPointerTy())
    return nullptr;
  const StoreInst *definition = nullptr;
  for (const User *user : slot->users()) {
    if (const auto *store = dyn_cast<StoreInst>(user)) {
      if (store->getPointerOperand() != slot || definition)
        return nullptr;
      definition = store;
    } else if (const auto *read = dyn_cast<LoadInst>(user)) {
      if (read->getPointerOperand() != slot)
        return nullptr;
    } else if (!isa<DbgInfoIntrinsic>(user)) {
      return nullptr;
    }
  }
  if (!definition || !dominators.dominates(definition, load))
    return nullptr;
  const auto *argument =
      dyn_cast<Argument>(definition->getValueOperand()->stripPointerCasts());
  return argument && argument->getParent() == &function ? argument : nullptr;
}

// Exact standard memory API roles; nullable buffers are only treated as read
// when a retained nonzero transfer requires the buffer to be dereferenced.
std::vector<unsigned> memoryArguments(const CallBase &call) {
  if (const auto *transfer = dyn_cast<MemTransferInst>(&call)) {
    const auto *length = dyn_cast<ConstantInt>(transfer->getLength());
    return length && !length->isZero() ? std::vector<unsigned>{0, 1}
                                       : std::vector<unsigned>{};
  }
  if (const auto *set = dyn_cast<MemSetInst>(&call)) {
    const auto *length = dyn_cast<ConstantInt>(set->getLength());
    return length && !length->isZero() ? std::vector<unsigned>{0}
                                       : std::vector<unsigned>{};
  }
  const Function *callee = ValueFacts::callee(call);
  if (!callee || call.arg_size() < 3)
    return {};
  if (!ValueFacts::hasLibraryName(*callee, "memcpy", true) &&
      !ValueFacts::hasLibraryName(*callee, "memmove", true) &&
      !ValueFacts::hasLibraryName(*callee, "bcopy"))
    return {};
  const auto *length = dyn_cast<ConstantInt>(call.getArgOperand(2));
  return length && !length->isZero() ? std::vector<unsigned>{0, 1}
                                     : std::vector<unsigned>{};
}

void refine(State &state, const Value *pointer, bool nonnull,
            const NullableFunctions &nullable) {
  NullFact fact = pointerFact(pointer, state, nullable);
  unsigned keep = nonnull ? NonNull : Null;
  if (!(fact.bits & UnknownNull) && !(fact.bits & keep)) {
    state.reachable = false;
    return;
  }
  const Value *identity = fact.identity;
  fact.bits = keep;
  state.pointers[pointer] = fact;
  if (!identity)
    return;
  for (auto &entry : state.pointers)
    if (entry.second.identity == identity)
      entry.second.bits = keep;
  for (auto &entry : state.cells)
    if (entry.second.pointer.identity == identity)
      entry.second.pointer.bits = keep;
}

void refineCondition(State &state, const Value *condition, bool truth,
                     const NullableFunctions &nullable, unsigned depth = 0) {
  if (depth > 8)
    return;
  if (const auto *constant = integerFact(condition, state)) {
    if (constant->isZero() == truth)
      state.reachable = false;
    return;
  }
  if (const auto *comparison = dyn_cast<ICmpInst>(condition)) {
    if (!comparison->isEquality())
      return;
    const Value *pointer = nullptr;
    if (isa<ConstantPointerNull>(comparison->getOperand(0)))
      pointer = comparison->getOperand(1);
    else if (isa<ConstantPointerNull>(comparison->getOperand(1)))
      pointer = comparison->getOperand(0);
    if (pointer)
      refine(state, pointer,
             truth == (comparison->getPredicate() == ICmpInst::ICMP_NE),
             nullable);
    return;
  }
  if (const auto *binary = dyn_cast<BinaryOperator>(condition)) {
    if ((binary->getOpcode() == Instruction::And && truth) ||
        (binary->getOpcode() == Instruction::Or && !truth)) {
      refineCondition(state, binary->getOperand(0), truth, nullable, depth + 1);
      refineCondition(state, binary->getOperand(1), truth, nullable, depth + 1);
    } else if (binary->getOpcode() == Instruction::Xor) {
      if (const auto *one = dyn_cast<ConstantInt>(binary->getOperand(1)))
        if (one->isOne())
          refineCondition(state, binary->getOperand(0), !truth, nullable,
                          depth + 1);
    }
  }
}

PointerNullState publicNull(unsigned bits) {
  if (bits == Null)
    return PointerNullState::Null;
  if (bits == NonNull)
    return PointerNullState::NonNull;
  if (bits == (Null | NonNull))
    return PointerNullState::Nullable;
  return PointerNullState::Unknown;
}

InitializationState publicInit(unsigned bits) {
  if (bits == Uninit)
    return InitializationState::Uninitialized;
  if (bits == Init)
    return InitializationState::Initialized;
  if (bits == (Uninit | Init))
    return InitializationState::MaybeUninitialized;
  return InitializationState::Unknown;
}

void addFinding(StateQueryResult &result, const std::set<std::string> &selected,
                const std::string &id, const std::string &message,
                const Instruction &site,
                const std::set<const Instruction *> &evidence) {
  if (!selected.empty() && !selected.count(id))
    return;
  StateFinding finding{id, message, &site, {}};
  finding.evidence.assign(evidence.begin(), evidence.end());
  result.findings.push_back(std::move(finding));
}

void observe(const Instruction &instruction, const State &state,
             const DataLayout &layout, const NullableFunctions &nullable,
             StateQueryResult &result) {
  std::set<const Value *> operands;
  for (const Use &operand : instruction.operands())
    if (operand->getType()->isPointerTy() && !isa<Function>(operand.get()))
      operands.insert(operand.get());
  for (const Value *pointer : operands) {
    PointerStateFact fact;
    fact.pointer = pointer;
    fact.nullness = publicNull(pointerFact(pointer, state, nullable).bits);
    if (const AllocaInst *cell = scalarCell(pointer, layout)) {
      auto found = state.cells.find(cell);
      if (found != state.cells.end())
        fact.pointee_initialization = publicInit(found->second.initialization);
    }
    result.states[&instruction].push_back(fact);
  }
}

// For cross-call diagnostics require a retained parameter dereference on every
// normal return, rather than assuming that an arbitrary callee reads a pointer.
ParameterReads summarizeParameterReads(const Module &module) {
  ParameterReads result;
  for (const Function &function : module) {
    if (function.isDeclaration())
      continue;
    DominatorTree dominators(const_cast<Function &>(function));
    std::vector<const ReturnInst *> returns;
    for (const BasicBlock &block : function)
      if (const auto *ret = dyn_cast<ReturnInst>(block.getTerminator()))
        if (dominators.isReachableFromEntry(&block))
          returns.push_back(ret);
    if (returns.empty())
      continue;
    for (const BasicBlock &block : function)
      for (const Instruction &instruction : block) {
        const Value *pointer = nullptr;
        if (const auto *load = dyn_cast<LoadInst>(&instruction))
          pointer = load->getPointerOperand();
        if (const auto *store = dyn_cast<StoreInst>(&instruction))
          pointer = store->getPointerOperand();
        if (!pointer)
          continue;
        const auto *argument = parameterOrigin(pointer, function, dominators);
        if (!argument || argument->getParent() != &function)
          continue;
        if (std::all_of(returns.begin(), returns.end(),
                        [&](const ReturnInst *ret) {
                          return dominators.dominates(&instruction, ret);
                        }))
          result[&function].emplace(argument->getArgNo(), &instruction);
      }
  }
  return result;
}

struct FunctionRun {
  bool returns_null = false;
  bool converged = true;
};

FunctionRun analyzeFunction(const Function &function,
                            const NullableFunctions &nullable,
                            const ParameterReads &reads,
                            const std::set<std::string> &selected,
                            StateQueryResult *result) {
  const DataLayout &layout = function.getParent()->getDataLayout();
  std::map<const BasicBlock *, State> inputs;
  std::map<const BasicBlock *, State> outputs;
  std::map<std::pair<const BasicBlock *, const BasicBlock *>, State> edges;
  std::deque<const BasicBlock *> queue;
  std::set<const BasicBlock *> queued;
  State entry;
  entry.reachable = true;
  for (const Argument &argument : function.args())
    if (argument.getType()->isPointerTy())
      entry.pointers[&argument] = {
          argument.hasNonNullAttr() ? NonNull : UnknownNull, &argument, {}};
  inputs[&function.getEntryBlock()] = entry;
  queue.push_back(&function.getEntryBlock());
  queued.insert(&function.getEntryBlock());
  const size_t limit = std::max<size_t>(1024, function.size() * 128);
  size_t visits = 0;
  FunctionRun run;
  // Mixed paths require a small, explicit coverage boundary until relational
  // scalar predicates and iteration-count proofs are available. A may-merge
  // across a loop or repeated scalar guard is not a feasible uninitialized
  // witness by itself.
  DominatorTree dominators(const_cast<Function &>(function));
  LoopInfo loops(dominators);
  unsigned conditional_branches = 0;
  bool has_switch = false;
  for (const BasicBlock &block : function) {
    if (const auto *branch = dyn_cast<BranchInst>(block.getTerminator()))
      conditional_branches += branch->isConditional();
    has_switch |= isa<SwitchInst>(block.getTerminator());
  }
  bool mixed_paths_supported =
      loops.empty() && !has_switch && conditional_branches == 1;

  auto transfer = [&](const BasicBlock &block, State state, bool emit) {
    if (!state.reachable)
      return state;
    for (const Instruction &instruction : block) {
      if (const auto *phi = dyn_cast<PHINode>(&instruction)) {
        if (phi->getType()->isIntegerTy()) {
          const ConstantInt *constant = nullptr;
          bool first = true;
          for (unsigned i = 0; i < phi->getNumIncomingValues(); ++i) {
            auto edge = edges.find({phi->getIncomingBlock(i), &block});
            if (edge == edges.end() || !edge->second.reachable)
              continue;
            const ConstantInt *incoming =
                integerFact(phi->getIncomingValue(i), edge->second);
            if (first)
              constant = incoming;
            else if (constant != incoming)
              constant = nullptr;
            first = false;
          }
          state.integers[phi] = constant;
        }
        if (!phi->getType()->isPointerTy())
          continue;
        NullFact fact;
        bool first = true;
        for (unsigned i = 0; i < phi->getNumIncomingValues(); ++i) {
          auto edge = edges.find({phi->getIncomingBlock(i), &block});
          if (edge == edges.end() || !edge->second.reachable)
            continue;
          NullFact incoming =
              pointerFact(phi->getIncomingValue(i), edge->second, nullable);
          fact = first ? incoming : mergeFact(fact, incoming);
          first = false;
        }
        if (!first) {
          if (!fact.identity)
            fact.identity = phi;
          state.pointers[phi] = fact;
        }
        continue;
      }
      if (emit)
        observe(instruction, state, layout, nullable, *result);
      if (const auto *alloca = dyn_cast<AllocaInst>(&instruction)) {
        if (scalarCell(alloca, layout))
          state.cells[alloca] = {Uninit, {}, alloca};
        state.pointers[alloca] = {NonNull, alloca, {}};
        continue;
      }
      const Value *dereference = nullptr;
      if (const auto *load = dyn_cast<LoadInst>(&instruction))
        dereference = load->getPointerOperand();
      if (const auto *store = dyn_cast<StoreInst>(&instruction))
        dereference = store->getPointerOperand();
      if (dereference && emit) {
        NullFact fact = pointerFact(dereference, state, nullable);
        if (isNullable(fact))
          addFinding(*result, selected, "cpp/missing-null-test",
                     "Pointer may be null on a reaching CFG edge before this "
                     "dereference.",
                     instruction, fact.origins);
      }
      if (const auto *load = dyn_cast<LoadInst>(&instruction)) {
        const AllocaInst *cell =
            scalarCell(load->getPointerOperand(), layout, load->getType());
        auto found = state.cells.find(cell);
        if (cell && found != state.cells.end()) {
          if (emit && !load->use_empty() &&
              found->second.initialization == Uninit)
            addFinding(*result, selected, "cpp/uninitialized-local",
                       "Scalar stack cell is uninitialized before this read on "
                       "all tracked predecessors.",
                       instruction, {cell});
          else if (emit && mixed_paths_supported && !load->use_empty() &&
                   found->second.initialization == (Uninit | Init))
            addFinding(*result, selected, "cpp/not-initialised",
                       "A reaching CFG predecessor reads this scalar stack "
                       "cell before initialization.",
                       instruction, {cell});
          if (load->getType()->isPointerTy())
            state.pointers[load] = found->second.pointer;
          if (load->getType()->isIntegerTy())
            state.integers[load] = found->second.integer;
        } else if (load->getType()->isPointerTy()) {
          state.pointers[load] = {};
        } else if (load->getType()->isIntegerTy()) {
          state.integers[load] = nullptr;
        }
        continue;
      }
      if (const auto *store = dyn_cast<StoreInst>(&instruction)) {
        if (const AllocaInst *cell =
                scalarCell(store->getPointerOperand(), layout,
                           store->getValueOperand()->getType())) {
          Cell &value = state.cells[cell];
          value.initialization =
              isa<UndefValue>(store->getValueOperand()) ||
                      isa<PoisonValue>(store->getValueOperand())
                  ? Uninit
                  : Init;
          value.pointer =
              pointerFact(store->getValueOperand(), state, nullable);
          if (value.pointer.bits == Null)
            value.pointer.origins.insert(store);
          value.last_write = store;
          value.integer = integerFact(store->getValueOperand(), state);
        } else if (const auto *root = dyn_cast<AllocaInst>(
                       getUnderlyingObject(store->getPointerOperand()))) {
          auto found = state.cells.find(root);
          if (found != state.cells.end()) {
            bool escaped = found->second.escaped;
            found->second = {};
            found->second.escaped = escaped;
          }
        } else {
          // An unresolved indirect store may update any cell whose address
          // has been copied into another pointer or escaped the function.
          for (auto &cell : state.cells)
            if (cell.second.escaped) {
              cell.second = {};
              cell.second.escaped = true;
            }
        }
        // An address held in another local pointer also introduces aliases;
        // unresolved writes must not leave a definite uninitialized fact.
        if (store->getValueOperand()->getType()->isPointerTy()) {
          const auto *root = dyn_cast<AllocaInst>(
              getUnderlyingObject(store->getValueOperand()));
          auto found = state.cells.find(root);
          if (found != state.cells.end())
            found->second.escaped = true;
        }
        continue;
      }
      if (const auto *call = dyn_cast<CallBase>(&instruction)) {
        if (const auto *assume = dyn_cast<IntrinsicInst>(call))
          if (assume->getIntrinsicID() == Intrinsic::assume) {
            refineCondition(state, assume->getArgOperand(0), true, nullable);
            if (!state.reachable)
              return state;
            continue;
          }
        if (const auto *variadic = dyn_cast<IntrinsicInst>(call))
          if (variadic->getIntrinsicID() == Intrinsic::vastart ||
              variadic->getIntrinsicID() == Intrinsic::vacopy) {
            const auto *root = dyn_cast<AllocaInst>(
                getUnderlyingObject(variadic->getArgOperand(0)));
            auto found = state.cells.find(root);
            if (found != state.cells.end()) {
              found->second.initialization = Init;
              found->second.pointer = {};
              found->second.last_write = variadic;
            }
            continue;
          }
        if (const auto *lifetime = dyn_cast<IntrinsicInst>(call))
          if (lifetime->getIntrinsicID() == Intrinsic::lifetime_start ||
              lifetime->getIntrinsicID() == Intrinsic::lifetime_end) {
            const auto *root = dyn_cast<AllocaInst>(
                getUnderlyingObject(lifetime->getArgOperand(1)));
            auto found = state.cells.find(root);
            if (found != state.cells.end()) {
              found->second.initialization =
                  lifetime->getIntrinsicID() == Intrinsic::lifetime_start
                      ? Uninit
                      : UnknownInit;
              found->second.pointer = {};
              found->second.last_write = lifetime;
            }
            continue;
          }
        const Function *callee = ValueFacts::callee(*call);
        if (emit)
          for (unsigned argument : memoryArguments(*call)) {
            NullFact fact =
                pointerFact(call->getArgOperand(argument), state, nullable);
            if (isNullable(fact))
              addFinding(*result, selected, "cpp/missing-null-test",
                         "A nonzero memory transfer dereferences a possibly "
                         "null buffer.",
                         instruction, fact.origins);
          }
        if (emit && callee) {
          auto summary = reads.find(callee);
          if (summary != reads.end())
            for (const auto &parameter : summary->second)
              if (parameter.first < call->arg_size()) {
                NullFact fact = pointerFact(
                    call->getArgOperand(parameter.first), state, nullable);
                // This rule specifically concerns a result of a nullable call.
                if (isNullable(fact) && hasCallOrigin(fact)) {
                  fact.origins.insert(parameter.second);
                  addFinding(*result, selected, "cpp/deref-null-result",
                             "Nullable function result reaches a parameter "
                             "dereferenced on every normal callee return.",
                             instruction, fact.origins);
                } else if (isNullable(fact)) {
                  fact.origins.insert(parameter.second);
                  addFinding(*result, selected, "cpp/missing-null-test",
                             "A possibly null argument reaches an "
                             "unconditional callee dereference.",
                             instruction, fact.origins);
                }
              }
        }
        if (const auto *memory = dyn_cast<MemIntrinsic>(call)) {
          const auto *root =
              dyn_cast<AllocaInst>(getUnderlyingObject(memory->getRawDest()));
          auto found = state.cells.find(root);
          if (found != state.cells.end()) {
            bool escaped = found->second.escaped;
            const auto *length = dyn_cast<ConstantInt>(memory->getLength());
            const AllocaInst *exact = scalarCell(memory->getRawDest(), layout);
            if (length && exact && length->getValue().getActiveBits() <= 64 &&
                length->getZExtValue() ==
                    layout.getTypeAllocSize(exact->getAllocatedType())) {
              found->second = {Init, {}, memory};
              if (const auto *set = dyn_cast<MemSetInst>(memory))
                if (const auto *byte = dyn_cast<ConstantInt>(set->getValue()))
                  if (byte->isZero() &&
                      exact->getAllocatedType()->isPointerTy())
                    found->second.pointer = {Null, set, {set}};
              if (const auto *copy = dyn_cast<MemTransferInst>(memory)) {
                const auto *source = scalarCell(copy->getRawSource(), layout);
                auto source_cell = state.cells.find(source);
                if (source_cell != state.cells.end())
                  found->second = source_cell->second;
              }
            } else
              found->second = {};
            found->second.escaped |= escaped;
          }
        } else if (!isa<DbgInfoIntrinsic>(call) &&
                   (!callee || !callee->isIntrinsic()) &&
                   !call->onlyReadsMemory()) {
          // Passing a tracked cell to an unknown writer invalidates it. This
          // suppresses unsupported output-parameter and alias assumptions.
          for (auto &cell : state.cells)
            if (cell.second.escaped) {
              cell.second = {};
              cell.second.escaped = true;
            }
          for (const Use &argument : call->args())
            if (argument->getType()->isPointerTy()) {
              const auto *root =
                  dyn_cast<AllocaInst>(getUnderlyingObject(argument.get()));
              auto found = state.cells.find(root);
              if (found != state.cells.end()) {
                found->second = {};
                found->second.escaped = true;
              }
            }
        }
        if (call->getType()->isPointerTy())
          state.pointers[call] = pointerFact(call, State{}, nullable);
        continue;
      }
      if (instruction.getType()->isPointerTy()) {
        state.pointers.erase(&instruction);
        state.pointers[&instruction] =
            pointerFact(&instruction, state, nullable);
      }
      if (instruction.getType()->isIntegerTy()) {
        state.integers.erase(&instruction);
        state.integers[&instruction] = integerFact(&instruction, state);
      }
    }
    return state;
  };

  while (!queue.empty()) {
    if (++visits > limit) {
      run.converged = false;
      break;
    }
    const BasicBlock *block = queue.front();
    queue.pop_front();
    queued.erase(block);
    State state = transfer(*block, inputs[block], false);
    outputs[block] = state;
    for (const BasicBlock *successor : successors(block)) {
      State edge_state = state;
      if (const auto *branch = dyn_cast<BranchInst>(block->getTerminator()))
        if (branch->isConditional())
          refineCondition(edge_state, branch->getCondition(),
                          branch->getSuccessor(0) == successor, nullable);
      auto key = std::make_pair(block, successor);
      if (edges.count(key) && edges[key] == edge_state)
        continue;
      edges[key] = edge_state;
      State incoming;
      if (successor == &function.getEntryBlock())
        incoming = entry;
      for (const BasicBlock *predecessor : predecessors(successor)) {
        auto edge = edges.find({predecessor, successor});
        if (edge != edges.end())
          mergeState(incoming, edge->second);
      }
      // PHIs use individual predecessor-edge states. Their inputs can change
      // even when the merged block state is identical (e.g. a new true/false
      // incoming value), so every changed edge must schedule the successor.
      inputs[successor] = std::move(incoming);
      if (queued.insert(successor).second)
        queue.push_back(successor);
    }
  }
  // Emit only after convergence. Transient worklist states are not findings.
  if (run.converged)
    for (const BasicBlock &block : function) {
      auto input = inputs.find(&block);
      if (input == inputs.end() || !input->second.reachable)
        continue;
      State final = transfer(block, input->second, result != nullptr);
      if (const auto *ret = dyn_cast<ReturnInst>(block.getTerminator()))
        if (ret->getReturnValue() &&
            ret->getReturnValue()->getType()->isPointerTy() &&
            isNullable(pointerFact(ret->getReturnValue(), final, nullable)))
          run.returns_null = true;
    }
  return run;
}

} // namespace

PointerStateFact StateQueryResult::stateBefore(const Instruction &instruction,
                                               const Value &pointer) const {
  auto found = states.find(&instruction);
  if (found != states.end())
    for (const auto &fact : found->second)
      if (fact.pointer == &pointer)
        return fact;
  return {&pointer, PointerNullState::Unknown, InitializationState::Unknown};
}

const std::vector<StateRuleDescriptor> &StateQuery::catalog() {
  static const std::vector<StateRuleDescriptor> catalog = {
      {"cpp/missing-null-test", "recommendation", "Critical/MissingNullTest.ql",
       "Retained load/store dereferences of null literals or "
       "malloc/calloc/direct nullable function results, constant nonzero "
       "memory transfers, and null literals passed to unconditional "
       "direct-callee dereferences. CFG null guards, SSA copies and exact "
       "scalar pointer cells supported. Unknown aliases and function pointers "
       "excluded; unchecked allocation alone is not a finding."},
      {"cpp/deref-null-result", "recommendation",
       "experimental/Likely Bugs/DerefNullResult.ql",
       "Nullable direct-call result passed to a direct callee parameter whose "
       "retained load/store dominates every reachable normal return. CFG "
       "caller null guards suppress findings. SSA parameters and unmodified "
       "private parameter spills supported; indirect calls and conditional "
       "callee dereferences excluded."},
      {"cpp/uninitialized-local", "warning",
       "Likely Bugs/Memory Management/UninitializedLocal.ql",
       "Definite uninitialized full scalar stack-cell loads on all tracked "
       "predecessors, with strong stores and CFG joins. Arrays/aggregates, "
       "escaped/unknown-clobbered cells, source macro exceptions and "
       "optimized-away undefined reads excluded. Mixed "
       "initialized/uninitialized joins are reported separately by "
       "not-initialised."},
      {"cpp/not-initialised", "error", "Critical/NotInitialised.ql",
       "Local-only conditional subset: full scalar stack-cell reads with "
       "initialized and uninitialized reaching predecessors in acyclic "
       "functions with a single conditional branch and no switch. Strong "
       "updates and pointer-null guard pruning supported; definite reads are "
       "assigned to uninitialized-local to avoid duplicate diagnostics. Loops, "
       "repeated scalar predicates, globals and source declarations are "
       "excluded."}};
  return catalog;
}

StateQueryResult
StateQuery::analyze(const Module &module,
                    const std::vector<std::string> &rule_ids) const {
  StateQueryResult result;
  std::set<std::string> selected(rule_ids.begin(), rule_ids.end());
  NullableFunctions nullable;
  for (const Function &function : module)
    if (ValueFacts::hasLibraryName(function, "malloc", true) ||
        ValueFacts::hasLibraryName(function, "calloc", true))
      nullable.insert(&function);
  ParameterReads reads = summarizeParameterReads(module);
  // Grow null-return summaries across direct wrappers without mixing caller
  // arguments with callee return identities. Bound recursion conservatively.
  bool changed = true;
  unsigned rounds = 0;
  while (changed && rounds++ < 16) {
    changed = false;
    for (const Function &function : module) {
      if (function.isDeclaration() ||
          !function.getReturnType()->isPointerTy() || nullable.count(&function))
        continue;
      FunctionRun run =
          analyzeFunction(function, nullable, reads, selected, nullptr);
      result.convergence_limit_hit |= !run.converged;
      if (run.returns_null)
        changed |= nullable.insert(&function).second;
    }
  }
  result.convergence_limit_hit |= changed;
  for (const Function &function : module) {
    if (function.isDeclaration())
      continue;
    ++result.analyzed_functions;
    FunctionRun run =
        analyzeFunction(function, nullable, reads, selected, &result);
    result.convergence_limit_hit |= !run.converged;
  }
  return result;
}

} // namespace pdg
