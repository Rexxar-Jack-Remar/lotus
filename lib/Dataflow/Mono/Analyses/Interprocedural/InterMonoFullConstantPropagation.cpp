#include "Dataflow/Mono/Analyses/Interprocedural/InterMonoFullConstantPropagation.h"

#include "Dataflow/Mono/InterMonoProblem.h"
#include "Dataflow/Mono/Solver/InterMonoSolver.h"
#include "Alias/AliasAnalysisWrapper/AliasAnalysisWrapper.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"

#include <memory>

using namespace llvm;

namespace mono {
namespace {

FullConstantValue joinValues(const FullConstantValue &Lhs,
                             const FullConstantValue &Rhs) {
  if (Lhs.Tag == FullConstantTag::Bottom) {
    return Rhs;
  }
  if (Rhs.Tag == FullConstantTag::Bottom) {
    return Lhs;
  }
  if (Lhs.Tag == FullConstantTag::Const && Rhs.Tag == FullConstantTag::Const &&
      Lhs.ConstValue == Rhs.ConstValue) {
    return Lhs;
  }
  return FullConstantValue::top();
}

FullConstantPropagationState joinStates(const FullConstantPropagationState &Lhs,
                                        const FullConstantPropagationState &Rhs) {
  if (Lhs.Unreachable) {
    return Rhs;
  }
  if (Rhs.Unreachable) {
    return Lhs;
  }

  FullConstantPropagationState Out;
  Out.Unreachable = false;
  Out.Values = Lhs.Values;
  for (const auto &Entry : Rhs.Values) {
    auto It = Out.Values.find(Entry.first);
    if (It == Out.Values.end()) {
      Out.Values.insert(Entry);
    } else {
      It->second = joinValues(It->second, Entry.second);
    }
  }
  return Out;
}

FullConstantValue resolveValue(const FullConstantPropagationState &In,
                               const Value *V) {
  if (In.Unreachable) {
    return FullConstantValue::bottom();
  }

  if (auto *CI = dyn_cast<ConstantInt>(V)) {
    return FullConstantValue::constant(CI->getSExtValue());
  }

  auto It = In.Values.find(V);
  if (It != In.Values.end()) {
    return It->second;
  }

  return FullConstantValue::top();
}

FullConstantValue evalBinaryOp(unsigned Opcode, const FullConstantValue &Lhs,
                               const FullConstantValue &Rhs) {
  if (Lhs.Tag == FullConstantTag::Bottom || Rhs.Tag == FullConstantTag::Bottom) {
    return FullConstantValue::bottom();
  }
  if (Lhs.Tag != FullConstantTag::Const || Rhs.Tag != FullConstantTag::Const) {
    return FullConstantValue::top();
  }

  auto LV = Lhs.ConstValue;
  auto RV = Rhs.ConstValue;
  switch (Opcode) {
  case Instruction::Add:
    return FullConstantValue::constant(LV + RV);
  case Instruction::Sub:
    return FullConstantValue::constant(LV - RV);
  case Instruction::Mul:
    return FullConstantValue::constant(LV * RV);
  case Instruction::SDiv:
    if (RV == 0) {
      return FullConstantValue::top();
    }
    return FullConstantValue::constant(LV / RV);
  case Instruction::UDiv:
    if (RV == 0) {
      return FullConstantValue::top();
    }
    return FullConstantValue::constant(static_cast<uint64_t>(LV) /
                                       static_cast<uint64_t>(RV));
  case Instruction::SRem:
    if (RV == 0) {
      return FullConstantValue::top();
    }
    return FullConstantValue::constant(LV % RV);
  case Instruction::URem:
    if (RV == 0) {
      return FullConstantValue::top();
    }
    return FullConstantValue::constant(static_cast<uint64_t>(LV) %
                                       static_cast<uint64_t>(RV));
  default:
    return FullConstantValue::top();
  }
}

struct FullDomain : LLVMMonoAnalysisDomain<FullConstantPropagationState> {};

class InterMonoFullConstantPropagation
    : public InterMonoProblem<FullDomain> {
public:
  explicit InterMonoFullConstantPropagation(Function *Entry,
                                             lotus::AliasAnalysisWrapper *AA)
      : InterMonoProblem<FullDomain>(std::vector<Function *>{Entry}, AA),
        AA(AA) {}

  mono_container_t allTop() override {
    return mono_container_t{};
  }

  mono_container_t normalFlow(Instruction *Inst,
                              const mono_container_t &In) override {
    if (In.Unreachable) {
      return In;
    }

    mono_container_t Out = In;

    if (const auto *Alloca = dyn_cast<AllocaInst>(Inst)) {
      if (Alloca->getAllocatedType()->isIntegerTy()) {
        Out.Values[Alloca] = FullConstantValue::top();
      }
      return Out;
    }

    if (const auto *Store = dyn_cast<StoreInst>(Inst)) {
      if (!Store->getValueOperand()->getType()->isIntegerTy()) {
        return Out;
      }
      auto *Ptr = Store->getPointerOperand();
      Out.Values[Ptr] = resolveValue(In, Store->getValueOperand());
      writeAliases(Ptr, Out.Values[Ptr], Out);
      return Out;
    }

    if (const auto *Load = dyn_cast<LoadInst>(Inst)) {
      auto It = In.Values.find(Load->getPointerOperand());
      if (It != In.Values.end()) {
        Out.Values[Load] = It->second;
      } else {
        Out.Values[Load] = resolveAliasValue(Load->getPointerOperand(), In);
      }
      return Out;
    }

    if (const auto *Op = dyn_cast<BinaryOperator>(Inst)) {
      auto L = resolveValue(In, Op->getOperand(0));
      auto R = resolveValue(In, Op->getOperand(1));
      Out.Values[Op] = evalBinaryOp(Op->getOpcode(), L, R);
      return Out;
    }

    return Out;
  }

  mono_container_t merge(const mono_container_t &Lhs,
                         const mono_container_t &Rhs) override {
    return joinStates(Lhs, Rhs);
  }

  bool equal_to(const mono_container_t &Lhs,
                const mono_container_t &Rhs) override {
    if (Lhs.Unreachable != Rhs.Unreachable) {
      return false;
    }
    return Lhs.Values == Rhs.Values;
  }

  mono_container_t callFlow(Instruction *CallSite, Function *Callee,
                            const mono_container_t &In) override {
    mono_container_t Out;
    if (In.Unreachable) {
      return Out;
    }
    Out.Unreachable = false;

    auto *Call = dyn_cast_or_null<CallBase>(CallSite);
    if (Call == nullptr || Callee == nullptr) {
      return Out;
    }

    auto *FormalIt = Callee->arg_begin();
    for (auto &Actual : Call->args()) {
      if (FormalIt == Callee->arg_end()) {
        break;
      }
      if (FormalIt->getType()->isIntegerTy()) {
        Out.Values[&*FormalIt] = resolveValue(In, Actual.get());
      }
      ++FormalIt;
    }

    for (const auto &Entry : In.Values) {
      if (isa<GlobalValue>(Entry.first)) {
        Out.Values.insert(Entry);
      }
    }

    return Out;
  }

  mono_container_t returnFlow(Instruction *CallSite, Function *Callee,
                              Instruction *ExitStmt, Instruction *RetSite,
                              const mono_container_t &In) override {
    (void)Callee;
    (void)RetSite;

    mono_container_t Out;
    if (In.Unreachable) {
      return Out;
    }
    Out.Unreachable = false;

    for (const auto &Entry : In.Values) {
      if (isa<GlobalValue>(Entry.first)) {
        Out.Values.insert(Entry);
      }
    }

    auto *Ret = dyn_cast_or_null<ReturnInst>(ExitStmt);
    auto *Call = dyn_cast_or_null<CallBase>(CallSite);
    if (Ret == nullptr || Call == nullptr) {
      return Out;
    }
    if (Call->getType()->isVoidTy()) {
      return Out;
    }

    auto *RetVal = Ret->getReturnValue();
    if (RetVal == nullptr || !RetVal->getType()->isIntegerTy()) {
      return Out;
    }

    Out.Values[CallSite] = resolveValue(In, RetVal);
    return Out;
  }

  mono_container_t callToRetFlow(Instruction *CallSite, Instruction *RetSite,
                                 ArrayRef<Function *> Callees,
                                 const mono_container_t &In) override {
    (void)RetSite;
    mono_container_t Out = In;
    if (In.Unreachable) {
      return Out;
    }

    auto *Call = dyn_cast_or_null<CallBase>(CallSite);
    if (Call == nullptr) {
      return Out;
    }
    if (Call->getType()->isVoidTy()) {
      return Out;
    }

    if (Callees.empty() || Callees.size() > 1) {
      Out.Values[CallSite] = FullConstantValue::top();
    }
    return Out;
  }

  std::unordered_map<Instruction *, mono_container_t> initialSeeds() override {
    std::unordered_map<Instruction *, mono_container_t> Seeds;
    Function *F = this->getEntryPoints().empty() ? nullptr : this->getEntryPoints().front();
    if (F == nullptr || F->empty()) {
      return Seeds;
    }
    mono_container_t Init;
    Init.Unreachable = false;
    Seeds[&F->getEntryBlock().front()] = Init;
    return Seeds;
  }

private:
  lotus::AliasAnalysisWrapper *AA;

  FullConstantValue resolveAliasValue(const Value *Ptr,
                                      const mono_container_t &In) const {
    if (AA == nullptr || !AA->isInitialized() || Ptr == nullptr ||
        !Ptr->getType()->isPointerTy()) {
      return FullConstantValue::top();
    }
    std::vector<const Value *> Aliases;
    if (!AA->getAliasSet(Ptr, Aliases)) {
      return FullConstantValue::top();
    }
    bool Found = false;
    FullConstantValue Val = FullConstantValue::top();
    for (const auto *Alias : Aliases) {
      auto It = In.Values.find(Alias);
      if (It == In.Values.end()) {
        continue;
      }
      if (!Found) {
        Val = It->second;
        Found = true;
      } else if (!(Val == It->second)) {
        return FullConstantValue::top();
      }
    }
    return Found ? Val : FullConstantValue::top();
  }

  void writeAliases(const Value *Ptr, const FullConstantValue &Val,
                    mono_container_t &Out) const {
    if (AA == nullptr || !AA->isInitialized() || Ptr == nullptr ||
        !Ptr->getType()->isPointerTy()) {
      return;
    }
    std::vector<const Value *> Aliases;
    if (!AA->getAliasSet(Ptr, Aliases)) {
      return;
    }
    for (const auto *Alias : Aliases) {
      Out.Values[Alias] = Val;
    }
  }
};

} // namespace

InterMonoFullConstantPropagationAnalysisResult
runInterMonoFullConstantPropagation(Function *Entry) {
  InterMonoFullConstantPropagationAnalysisResult Result;
  if (Entry == nullptr || Entry->isDeclaration()) {
    return Result;
  }

  auto AA = std::make_unique<lotus::AliasAnalysisWrapper>(
      *Entry->getParent(),
      lotus::AAConfig(lotus::AAConfig::Implementation::BasicAA,
                      lotus::AAConfig::ContextSensitivity::None, 0, true,
                      lotus::AAConfig::Solver::Default));
  InterMonoFullConstantPropagation Problem(Entry, AA.get());
  InterMonoSolver<FullDomain, kDefaultFullConstantPropagationCallStringLength> Solver(Problem);
  Solver.solve();

  if (const auto *Raw = Solver.getResults()) {
    Result.Results =
        std::make_unique<InterMonoFullConstantPropagationResult>(*Raw);
  }
  return Result;
}

} // namespace mono
