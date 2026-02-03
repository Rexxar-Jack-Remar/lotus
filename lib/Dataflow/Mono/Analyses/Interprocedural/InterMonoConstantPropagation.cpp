#include "Dataflow/Mono/Analyses/Interprocedural/InterMonoConstantPropagation.h"

#include "Dataflow/Mono/InterMonoProblem.h"
#include "Dataflow/Mono/Solver/InterMonoSolver.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"

using namespace llvm;

namespace mono {
namespace {

ConstantPropagationValue makeTop() {
  return ConstantPropagationValue{ConstantPropagationTag::Top, 0};
}

ConstantPropagationValue makeBottom() {
  return ConstantPropagationValue{ConstantPropagationTag::Bottom, 0};
}

ConstantPropagationValue makeConst(int64_t Value) {
  return ConstantPropagationValue{ConstantPropagationTag::Const, Value};
}

bool isBottom(const ConstantPropagationValue &V) {
  return V.Tag == ConstantPropagationTag::Bottom;
}

bool isConst(const ConstantPropagationValue &V) {
  return V.Tag == ConstantPropagationTag::Const;
}

bool equalValue(const ConstantPropagationValue &Lhs,
                const ConstantPropagationValue &Rhs) {
  return Lhs.Tag == Rhs.Tag && Lhs.ConstValue == Rhs.ConstValue;
}

ConstantPropagationValue resolveValue(const ConstantPropagationMap &In,
                                      const Value *V) {
  if (auto *CI = dyn_cast<ConstantInt>(V)) {
    return makeConst(CI->getSExtValue());
  }

  auto It = In.find(V);
  if (It != In.end()) {
    return It->second;
  }

  return makeTop();
}

ConstantPropagationValue evalBinaryOp(unsigned Opcode,
                                      const ConstantPropagationValue &Lhs,
                                      const ConstantPropagationValue &Rhs) {
  if (!isConst(Lhs) || !isConst(Rhs)) {
    return makeBottom();
  }

  auto LV = Lhs.ConstValue;
  auto RV = Rhs.ConstValue;
  switch (Opcode) {
  case Instruction::Add:
    return makeConst(LV + RV);
  case Instruction::Sub:
    return makeConst(LV - RV);
  case Instruction::Mul:
    return makeConst(LV * RV);
  case Instruction::SDiv:
    if (RV == 0) {
      return makeBottom();
    }
    return makeConst(LV / RV);
  case Instruction::UDiv:
    if (RV == 0) {
      return makeBottom();
    }
    return makeConst(static_cast<uint64_t>(LV) /
                     static_cast<uint64_t>(RV));
  case Instruction::SRem:
    if (RV == 0) {
      return makeBottom();
    }
    return makeConst(LV % RV);
  case Instruction::URem:
    if (RV == 0) {
      return makeBottom();
    }
    return makeConst(static_cast<uint64_t>(LV) %
                     static_cast<uint64_t>(RV));
  default:
    return makeBottom();
  }
}

class InterMonoConstantPropagation
    : public InterMonoProblem<ConstantPropagationDomain> {
public:
  explicit InterMonoConstantPropagation(Function *Entry)
      : InterMonoProblem<ConstantPropagationDomain>(
            std::vector<Function *>{Entry}) {}

  ConstantPropagationMap normalFlow(Instruction *Inst,
                                    const ConstantPropagationMap &In) override {
    ConstantPropagationMap Out = In;

    if (const auto *Alloca = dyn_cast<AllocaInst>(Inst)) {
      if (Alloca->getAllocatedType()->isIntegerTy()) {
        Out[Alloca] = makeTop();
      }
      return Out;
    }

    if (const auto *Store = dyn_cast<StoreInst>(Inst)) {
      auto *Ptr = Store->getPointerOperand();
      if (!Store->getValueOperand()->getType()->isIntegerTy()) {
        return Out;
      }

      auto Val = resolveValue(In, Store->getValueOperand());
      if (!isBottom(Val)) {
        Out[Ptr] = Val;
      }
      return Out;
    }

    if (const auto *Load = dyn_cast<LoadInst>(Inst)) {
      auto It = In.find(Load->getPointerOperand());
      if (It != In.end()) {
        Out[Load] = It->second;
      }
      return Out;
    }

    if (const auto *Op = dyn_cast<BinaryOperator>(Inst)) {
      auto Lhs = resolveValue(In, Op->getOperand(0));
      auto Rhs = resolveValue(In, Op->getOperand(1));
      Out[Op] = evalBinaryOp(Op->getOpcode(), Lhs, Rhs);
      return Out;
    }

    return Out;
  }

  ConstantPropagationMap merge(const ConstantPropagationMap &Lhs,
                               const ConstantPropagationMap &Rhs) override {
    ConstantPropagationMap Out;
    for (const auto &Entry : Lhs) {
      auto It = Rhs.find(Entry.first);
      if (It != Rhs.end() && equalValue(Entry.second, It->second)) {
        Out.insert(std::make_pair(Entry.first, Entry.second));
      }
    }
    return Out;
  }

  bool equal_to(const ConstantPropagationMap &Lhs,
                const ConstantPropagationMap &Rhs) override {
    return Lhs == Rhs;
  }

  ConstantPropagationMap callFlow(Instruction *CallSite, Function *Callee,
                                  const ConstantPropagationMap &In) override {
    ConstantPropagationMap Out;
    if (CallSite == nullptr || Callee == nullptr) {
      return Out;
    }

    auto *Call = dyn_cast<CallBase>(CallSite);
    if (Call == nullptr) {
      return Out;
    }

    // Map constants from actual args to formal args.
    auto *FormalIt = Callee->arg_begin();
    for (auto &Actual : Call->args()) {
      if (FormalIt == Callee->arg_end()) {
        break;
      }
      if (FormalIt->getType()->isIntegerTy()) {
        Out[&*FormalIt] = resolveValue(In, Actual.get());
      }
      ++FormalIt;
    }

    // Preserve globals (very conservative).
    for (const auto &Entry : In) {
      if (isa<GlobalValue>(Entry.first)) {
        Out.insert(Entry);
      }
    }

    return Out;
  }

  ConstantPropagationMap returnFlow(Instruction *CallSite, Function *Callee,
                                    Instruction *ExitStmt,
                                    Instruction *RetSite,
                                    const ConstantPropagationMap &In) override {
    (void)Callee;
    (void)RetSite;

    ConstantPropagationMap Out;
    for (const auto &Entry : In) {
      if (isa<GlobalValue>(Entry.first)) {
        Out.insert(Entry);
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

    Out[CallSite] = resolveValue(In, RetVal);
    return Out;
  }

  ConstantPropagationMap callToRetFlow(Instruction *CallSite,
                                       Instruction *RetSite,
                                       ArrayRef<Function *> Callees,
                                       const ConstantPropagationMap &In) override {
    (void)RetSite;
    ConstantPropagationMap Out = In;

    auto *Call = dyn_cast_or_null<CallBase>(CallSite);
    if (Call == nullptr) {
      return Out;
    }
    if (Call->getType()->isVoidTy()) {
      return Out;
    }

    // Unknown/indirect call: we lose constant information about the return value.
    if (Callees.empty()) {
      Out[CallSite] = makeBottom();
      return Out;
    }

    // Multiple potential callees: conservatively mark as unknown here. The
    // precise constant, if any, will be re-established by returnFlow + merge.
    if (Callees.size() > 1) {
      Out[CallSite] = makeBottom();
    }
    return Out;
  }

  std::unordered_map<Instruction *, ConstantPropagationMap>
  initialSeeds() override {
    std::unordered_map<Instruction *, ConstantPropagationMap> Seeds;
    Function *F = this->getEntryPoints().empty() ? nullptr
                                                 : this->getEntryPoints().front();
    if (F == nullptr || F->empty()) {
      return Seeds;
    }
    Seeds[&F->getEntryBlock().front()] = ConstantPropagationMap{};
    return Seeds;
  }
};

} // namespace

InterMonoConstantPropagationAnalysisResult
runInterMonoConstantPropagation(Function *Entry) {
  InterMonoConstantPropagationAnalysisResult Result;
  if (Entry == nullptr || Entry->isDeclaration()) {
    return Result;
  }

  InterMonoConstantPropagation Problem(Entry);
  InterMonoSolver<ConstantPropagationDomain, kDefaultConstantPropagationCallStringLength>
      Solver(Problem);
  Solver.solve();

  if (const auto *Raw = Solver.getResults()) {
    Result.Results = std::make_unique<InterMonoConstantPropagationResult>(*Raw);
  }
  return Result;
}

} // namespace mono

