#include "Dataflow/Mono/Analyses/Intraprocedural/IntraMonoUninitVariables.h"
#include "Dataflow/Mono/IntraMonoProblem.h"
#include "Dataflow/Mono/LLVMMonoAnalysisDomain.h"
#include "Dataflow/Mono/Solver/IntraMonoSolver.h"

#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Instructions.h"

#include <algorithm>
#include <set>

using namespace llvm;

namespace mono {
namespace {

struct UninitVariablesDomain : LLVMMonoAnalysisDomain<std::set<Value *>> {};

class UninitVariablesProblem : public IntraMonoProblem<UninitVariablesDomain> {
public:
  explicit UninitVariablesProblem(Function *F)
      : IntraMonoProblem<UninitVariablesDomain>({F}),
        DL(&F->getParent()->getDataLayout()) {}

  std::set<Value *> allTop() override { return {}; }

  std::set<Value *> normalFlow(Instruction *Inst,
                               const std::set<Value *> &In) override {
    std::set<Value *> Out = In;

    if (auto *Alloca = dyn_cast<AllocaInst>(Inst)) {
      Out.insert(Alloca);
      return Out;
    }

    if (auto *Store = dyn_cast<StoreInst>(Inst)) {
      auto *Ptr = Store->getPointerOperand();
      auto *Val = Store->getValueOperand();
      if (isa<UndefValue>(Val)) {
        markAliasUninit(Out, Ptr);
        return Out;
      }

      auto *StoredInst = dyn_cast<Instruction>(Val);
      if (StoredInst != nullptr && In.count(StoredInst)) {
        markAliasUninit(Out, Ptr);
      } else {
        clearAliasUninit(Out, Ptr);
      }
      return Out;
    }

    if (auto *Load = dyn_cast<LoadInst>(Inst)) {
      if (In.count(Load->getPointerOperand())) {
        Out.insert(Load);
      }
      return Out;
    }

    if (auto *Bitcast = dyn_cast<BitCastInst>(Inst)) {
      if (In.count(Bitcast->getOperand(0))) {
        Out.insert(Bitcast);
      }
      return Out;
    }

    if (auto *GEP = dyn_cast<GetElementPtrInst>(Inst)) {
      if (In.count(GEP->getPointerOperand())) {
        Out.insert(GEP);
      }
      return Out;
    }

    if (auto *Phi = dyn_cast<PHINode>(Inst)) {
      for (auto &IncomingUse : Phi->incoming_values()) {
        auto *Incoming = IncomingUse.get();
        if (In.count(Incoming)) {
          Out.insert(Phi);
          break;
        }
      }
      return Out;
    }

    if (auto *Select = dyn_cast<SelectInst>(Inst)) {
      if (In.count(Select->getTrueValue()) ||
          In.count(Select->getFalseValue())) {
        Out.insert(Select);
      }
      return Out;
    }

    if (auto *Call = dyn_cast<CallBase>(Inst)) {
      handleMemIntrinsics(Call, Out);
      return Out;
    }

    return Out;
  }

  std::set<Value *> merge(const std::set<Value *> &Lhs,
                          const std::set<Value *> &Rhs) override {
    std::set<Value *> Out;
    std::set_intersection(Lhs.begin(), Lhs.end(), Rhs.begin(), Rhs.end(),
                          std::inserter(Out, Out.begin()));
    return Out;
  }

  bool equal_to(const std::set<Value *> &Lhs,
                const std::set<Value *> &Rhs) override {
    return Lhs == Rhs;
  }

  std::unordered_map<Instruction *, std::set<Value *>> initialSeeds() override {
    std::unordered_map<Instruction *, std::set<Value *>> Seeds;
    auto *F = getEntryPoints().empty() ? nullptr : getEntryPoints().front();
    if (F == nullptr || F->empty()) {
      return Seeds;
    }
    Seeds[&F->getEntryBlock().front()] = allTop();
    return Seeds;
  }

private:
  const DataLayout *DL;

  const Value *getBaseObject(const Value *V) const {
    return llvm::getUnderlyingObject(V);
  }

  void clearAliasUninit(std::set<Value *> &Out, const Value *Ptr) const {
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

  static void markAliasUninit(std::set<Value *> &Out, Value *Ptr) {
    Out.insert(Ptr);
  }

  static bool isMemIntrinsic(Function *Callee, Intrinsic::ID ID) {
    return Callee != nullptr && Callee->getIntrinsicID() == ID;
  }

  void handleMemIntrinsics(CallBase *Call, std::set<Value *> &Out) {
    auto *Callee = Call->getCalledFunction();
    if (isMemIntrinsic(Callee, Intrinsic::memset)) {
      if (Call->arg_size() >= 2) {
        auto *Dest = Call->getArgOperand(0);
        auto *Val = Call->getArgOperand(1);
        if (!isa<UndefValue>(Val)) {
          clearAliasUninit(Out, Dest);
        } else {
          markAliasUninit(Out, Dest);
        }
      }
      return;
    }
    if (isMemIntrinsic(Callee, Intrinsic::memcpy) ||
        isMemIntrinsic(Callee, Intrinsic::memmove)) {
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

std::unique_ptr<DataFlowResult> runIntraMonoUninitVariables(Function *F) {
  if (F == nullptr || F->isDeclaration()) {
    return nullptr;
  }

  UninitVariablesProblem Problem(F);
  IntraMonoSolver<UninitVariablesDomain> Solver(Problem);
  Solver.solve();

  auto Result = std::make_unique<DataFlowResult>();
  for (auto &BB : *F) {
    for (auto &Inst : BB) {
      auto *I = &Inst;
      Result->IN(I) = Solver.getInResultsAt(I);
      Result->OUT(I) = Solver.getOutResultsAt(I);

      if (isa<AllocaInst>(I)) {
        Result->GEN(I).insert(I);
      }
      if (auto *Store = dyn_cast<StoreInst>(I)) {
        if (!isa<UndefValue>(Store->getValueOperand())) {
          Result->KILL(I).insert(Store->getPointerOperand());
        }
      }
    }
  }

  return Result;
}

} // namespace mono
