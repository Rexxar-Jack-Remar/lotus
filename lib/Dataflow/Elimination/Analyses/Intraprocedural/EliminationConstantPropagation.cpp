#include "Dataflow/Elimination/Analyses/Intraprocedural/EliminationConstantPropagation.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"

namespace elimination {
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

bool isTop(const ConstantPropagationValue &V) {
  return V.Tag == ConstantPropagationTag::Top;
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
                                      const llvm::Value *V) {
  if (auto *CI = llvm::dyn_cast<llvm::ConstantInt>(V)) {
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
  if (isTop(Lhs) || isTop(Rhs) || isBottom(Lhs) || isBottom(Rhs)) {
    return makeBottom();
  }

  if (!isConst(Lhs) || !isConst(Rhs)) {
    return makeBottom();
  }

  auto LV = Lhs.ConstValue;
  auto RV = Rhs.ConstValue;
  switch (Opcode) {
  case llvm::Instruction::Add:
    return makeConst(LV + RV);
  case llvm::Instruction::Sub:
    return makeConst(LV - RV);
  case llvm::Instruction::Mul:
    return makeConst(LV * RV);
  case llvm::Instruction::SDiv:
    if (RV == 0) {
      return makeBottom();
    }
    return makeConst(LV / RV);
  case llvm::Instruction::UDiv:
    if (RV == 0) {
      return makeBottom();
    }
    return makeConst(
        static_cast<uint64_t>(LV) / static_cast<uint64_t>(RV));
  case llvm::Instruction::SRem:
    if (RV == 0) {
      return makeBottom();
    }
    return makeConst(LV % RV);
  case llvm::Instruction::URem:
    if (RV == 0) {
      return makeBottom();
    }
    return makeConst(
        static_cast<uint64_t>(LV) % static_cast<uint64_t>(RV));
  default:
    return makeBottom();
  }
}

class ElimConstantPropagationProblem
    : public LLVMIntraEliminationProblem<ConstantPropagationMap> {
public:
  explicit ElimConstantPropagationProblem(llvm::Function *F)
      : LLVMIntraEliminationProblem<ConstantPropagationMap>(F) {}

  ConstantPropagationMap applyTransfer(const transfer_t &T,
                                       const ConstantPropagationMap &In)
      const override {
    auto *Inst = T;
    ConstantPropagationMap Out = In;
    if (Inst == nullptr) {
      return Out;
    }

    if (const auto *Alloca = llvm::dyn_cast<llvm::AllocaInst>(Inst)) {
      if (Alloca->getAllocatedType()->isIntegerTy()) {
        Out[Alloca] = makeTop();
      }
      return Out;
    }

    if (const auto *Store = llvm::dyn_cast<llvm::StoreInst>(Inst)) {
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

    if (const auto *Load = llvm::dyn_cast<llvm::LoadInst>(Inst)) {
      auto It = In.find(Load->getPointerOperand());
      if (It != In.end()) {
        Out[Load] = It->second;
      }
      return Out;
    }

    if (const auto *Op = llvm::dyn_cast<llvm::BinaryOperator>(Inst)) {
      auto Lhs = resolveValue(In, Op->getOperand(0));
      auto Rhs = resolveValue(In, Op->getOperand(1));
      Out[Op] = evalBinaryOp(Op->getOpcode(), Lhs, Rhs);
      return Out;
    }

    return Out;
  }

  ConstantPropagationMap meet(const ConstantPropagationMap &Lhs,
                              const ConstantPropagationMap &Rhs)
      const override {
    ConstantPropagationMap Out;
    for (const auto &Entry : Lhs) {
      auto It = Rhs.find(Entry.first);
      if (It != Rhs.end() && equalValue(Entry.second, It->second)) {
        Out.insert({Entry.first, Entry.second});
      }
    }
    return Out;
  }

  bool equal_to(const ConstantPropagationMap &Lhs,
                const ConstantPropagationMap &Rhs) const override {
    return Lhs == Rhs;
  }

  ConstantPropagationMap meetIdentity() const override {
    return ConstantPropagationMap{};
  }

  ConstantPropagationMap initialFact() const override {
    return ConstantPropagationMap{};
  }
};

} // namespace

ConstantPropagationResult
runIntraElimConstantPropagation(llvm::Function *F, EliminationOptions Opts) {
  if (F == nullptr || F->isDeclaration()) {
    return ConstantPropagationResult{};
  }

  ElimConstantPropagationProblem Problem(F);
  IntraEliminationSolver<LLVMEliminationDomain<ConstantPropagationMap>> Solver(
      Problem, Opts);
  Solver.solve();
  return Solver.getResults();
}

} // namespace elimination
