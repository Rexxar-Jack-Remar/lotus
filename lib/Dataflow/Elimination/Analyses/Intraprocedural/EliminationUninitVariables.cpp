#include "Dataflow/Elimination/Analyses/Intraprocedural/EliminationUninitVariables.h"

#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"

#include <algorithm>
#include <iterator>

namespace elimination {
namespace {

class ElimUninitVariablesProblem
    : public LLVMIntraEliminationProblem<UninitVariablesFact> {
public:
  explicit ElimUninitVariablesProblem(llvm::Function *F)
      : LLVMIntraEliminationProblem<UninitVariablesFact>(F),
        DL(F != nullptr ? &F->getParent()->getDataLayout() : nullptr) {}

  UninitVariablesFact applyTransfer(const transfer_t &T,
                                    const UninitVariablesFact &In)
      const override {
    auto *Inst = T;
    UninitVariablesFact Out = In;
    if (Inst == nullptr) {
      return Out;
    }

    if (auto *Alloca = llvm::dyn_cast<llvm::AllocaInst>(Inst)) {
      Out.insert(Alloca);
      return Out;
    }

    if (auto *Store = llvm::dyn_cast<llvm::StoreInst>(Inst)) {
      auto *Ptr = Store->getPointerOperand();
      auto *Val = Store->getValueOperand();
      if (llvm::isa<llvm::UndefValue>(Val)) {
        markAliasUninit(Out, Ptr);
        return Out;
      }

      auto *StoredInst = llvm::dyn_cast<llvm::Instruction>(Val);
      if (StoredInst != nullptr && In.count(StoredInst)) {
        markAliasUninit(Out, Ptr);
      } else {
        clearAliasUninit(Out, Ptr);
      }
      return Out;
    }

    if (auto *Load = llvm::dyn_cast<llvm::LoadInst>(Inst)) {
      if (In.count(Load->getPointerOperand())) {
        Out.insert(Load);
      }
      return Out;
    }

    if (auto *Bitcast = llvm::dyn_cast<llvm::BitCastInst>(Inst)) {
      if (In.count(Bitcast->getOperand(0))) {
        Out.insert(Bitcast);
      }
      return Out;
    }

    if (auto *GEP = llvm::dyn_cast<llvm::GetElementPtrInst>(Inst)) {
      if (In.count(GEP->getPointerOperand())) {
        Out.insert(GEP);
      }
      return Out;
    }

    if (auto *Phi = llvm::dyn_cast<llvm::PHINode>(Inst)) {
      for (auto &IncomingUse : Phi->incoming_values()) {
        auto *Incoming = IncomingUse.get();
        if (In.count(Incoming)) {
          Out.insert(Phi);
          break;
        }
      }
      return Out;
    }

    if (auto *Select = llvm::dyn_cast<llvm::SelectInst>(Inst)) {
      if (In.count(Select->getTrueValue()) ||
          In.count(Select->getFalseValue())) {
        Out.insert(Select);
      }
      return Out;
    }

    if (auto *Call = llvm::dyn_cast<llvm::CallBase>(Inst)) {
      handleMemIntrinsics(Call, Out);
      return Out;
    }

    return Out;
  }

  UninitVariablesFact meet(const UninitVariablesFact &Lhs,
                           const UninitVariablesFact &Rhs) const override {
    UninitVariablesFact Out;
    std::set_intersection(Lhs.begin(), Lhs.end(), Rhs.begin(), Rhs.end(),
                          std::inserter(Out, Out.begin()));
    return Out;
  }

  bool equal_to(const UninitVariablesFact &Lhs,
                const UninitVariablesFact &Rhs) const override {
    return Lhs == Rhs;
  }

  UninitVariablesFact meetIdentity() const override {
    return UninitVariablesFact{};
  }

  UninitVariablesFact initialFact() const override {
    return UninitVariablesFact{};
  }

private:
  const llvm::DataLayout *DL;

  const llvm::Value *getBaseObject(const llvm::Value *V) const {
    (void)DL;
    return llvm::getUnderlyingObject(V);
  }

  void clearAliasUninit(UninitVariablesFact &Out, const llvm::Value *Ptr) const {
    auto *Base = getBaseObject(Ptr);
    for (auto It = Out.begin(); It != Out.end();) {
      auto *Candidate = *It;
      if (Candidate == Ptr) {
        It = Out.erase(It);
        continue;
      }
      if (Candidate->getType()->isPointerTy() &&
          getBaseObject(Candidate) == Base) {
        It = Out.erase(It);
        continue;
      }
      ++It;
    }
  }

  static void markAliasUninit(UninitVariablesFact &Out, llvm::Value *Ptr) {
    Out.insert(Ptr);
  }

  static bool isMemIntrinsic(llvm::Function *Callee, llvm::Intrinsic::ID ID) {
    return Callee != nullptr && Callee->getIntrinsicID() == ID;
  }

  void handleMemIntrinsics(llvm::CallBase *Call, UninitVariablesFact &Out) const {
    auto *Callee = Call->getCalledFunction();
    if (isMemIntrinsic(Callee, llvm::Intrinsic::memset)) {
      if (Call->arg_size() >= 2) {
        auto *Dest = Call->getArgOperand(0);
        auto *Val = Call->getArgOperand(1);
        if (!llvm::isa<llvm::UndefValue>(Val)) {
          clearAliasUninit(Out, Dest);
        } else {
          markAliasUninit(Out, Dest);
        }
      }
      return;
    }
    if (isMemIntrinsic(Callee, llvm::Intrinsic::memcpy) ||
        isMemIntrinsic(Callee, llvm::Intrinsic::memmove)) {
      if (Call->arg_size() >= 2) {
        auto *Dest = Call->getArgOperand(0);
        auto *Src = Call->getArgOperand(1);
        if (Out.count(Src)) {
          markAliasUninit(Out, Dest);
        } else {
          clearAliasUninit(Out, Dest);
        }
      }
      return;
    }
  }
};

} // namespace

UninitVariablesResult runIntraElimUninitVariables(llvm::Function *F,
                                                 EliminationOptions Opts) {
  if (F == nullptr || F->isDeclaration()) {
    return UninitVariablesResult{};
  }

  ElimUninitVariablesProblem Problem(F);
  IntraEliminationSolver<LLVMEliminationDomain<UninitVariablesFact>> Solver(
      Problem, Opts);
  Solver.solve();
  return Solver.getResults();
}

} // namespace elimination
