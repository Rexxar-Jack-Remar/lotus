#include "Dataflow/Mono/Analyses/Intraprocedural/IntraMonoUninitVariables.h"
#include "Dataflow/Mono/IntraMonoProblem.h"
#include "Dataflow/Mono/LLVMMonoAnalysisDomain.h"
#include "Dataflow/Mono/Solver/IntraMonoSolver.h"
#include "Alias/AliasAnalysisWrapper/AliasAnalysisWrapper.h"

#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Instructions.h"

#include <algorithm>
#include <memory>
#include <set>

using namespace llvm;

namespace mono {
namespace {

struct UninitVariablesDomain : LLVMMonoAnalysisDomain<std::set<Value *>> {};

class UninitVariablesProblem : public IntraMonoProblem<UninitVariablesDomain> {
public:
  explicit UninitVariablesProblem(Function *F, lotus::AliasAnalysisWrapper *AA)
      : IntraMonoProblem<UninitVariablesDomain>({F}, AA),
        DL(&F->getParent()->getDataLayout()),
        AA(AA) {}

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

      if (isUninitValue(Val, In)) {
        markAliasUninit(Out, Ptr);
      } else {
        clearAliasUninit(Out, Ptr);
      }
      return Out;
    }

    if (auto *Load = dyn_cast<LoadInst>(Inst)) {
      if (isUninitValue(Load->getPointerOperand(), In)) {
        Out.insert(Load);
      }
      return Out;
    }

    if (auto *Bitcast = dyn_cast<BitCastInst>(Inst)) {
      if (isUninitValue(Bitcast->getOperand(0), In)) {
        Out.insert(Bitcast);
      }
      return Out;
    }

    if (auto *GEP = dyn_cast<GetElementPtrInst>(Inst)) {
      if (isUninitValue(GEP->getPointerOperand(), In)) {
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
      if (isUninitValue(Select->getTrueValue(), In) ||
          isUninitValue(Select->getFalseValue(), In)) {
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
  lotus::AliasAnalysisWrapper *AA;

  const Value *getBaseObject(const Value *V) const {
    return llvm::getUnderlyingObject(V);
  }

  bool isUninitValue(const Value *V, const std::set<Value *> &In) const {
    if (V == nullptr) {
      return false;
    }
    if (In.count(const_cast<Value *>(V))) {
      return true;
    }
    if (AA == nullptr || !AA->isInitialized()) {
      return false;
    }
    if (!V->getType()->isPointerTy()) {
      return false;
    }
    std::vector<const Value *> Aliases;
    if (!AA->getAliasSet(V, Aliases)) {
      return false;
    }
    for (const auto *Alias : Aliases) {
      if (In.count(const_cast<Value *>(Alias))) {
        return true;
      }
    }
    return false;
  }

  void clearAliasUninit(std::set<Value *> &Out, const Value *Ptr) const {
    if (AA != nullptr && AA->isInitialized() && Ptr != nullptr &&
        Ptr->getType()->isPointerTy()) {
      std::vector<const Value *> Aliases;
      if (AA->getAliasSet(Ptr, Aliases)) {
        for (const auto *Alias : Aliases) {
          Out.erase(const_cast<Value *>(Alias));
        }
      }
      Out.erase(const_cast<Value *>(Ptr));
      return;
    }
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
        if (isUninitValue(Src, Out)) {
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

  auto AA = std::make_unique<lotus::AliasAnalysisWrapper>(
      *F->getParent(),
      lotus::AAConfig(lotus::AAConfig::Implementation::BasicAA,
                      lotus::AAConfig::ContextSensitivity::None, 0, true,
                      lotus::AAConfig::Solver::Default));
  UninitVariablesProblem Problem(F, AA.get());
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
