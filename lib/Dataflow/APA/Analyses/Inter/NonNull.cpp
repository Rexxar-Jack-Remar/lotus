#include "Dataflow/APA/Analyses/Inter/NonNull.h"

#include "llvm/Analysis/AssumeBundleQueries.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"

#include "Dataflow/APA/LLVM/InterProblem.h"
#include "Dataflow/APA/Solver/Inter/ExpandedSolver.h"
#include "Dataflow/APA/Analyses/Inter/FlowHelpers.h"

#include <map>
#include <memory>

namespace elimination {
namespace {

struct InterNonNullAnalysisTypes {
  using n_t = llvm::Instruction *;
  using fact_t = NonNullFact;
  using transfer_t = NonNullEdgeTransfer;
  using f_t = llvm::Function *;
  using i_t = dataflow::controlflow::InterCFG;
  using abstract_domain_t = NonNullDomain;
};

NonNullDomain makeDomain(llvm::Function *Entry) {
  NonNullFact Universe;
  auto *M = Entry != nullptr ? Entry->getParent() : nullptr;
  if (M == nullptr)
    return NonNullDomain(Universe);
  for (auto &F : *M) {
    for (auto &Arg : F.args()) {
      if (Arg.getType()->isPointerTy())
        Universe.insert(&Arg);
    }
    for (auto &BB : F) {
      for (auto &I : BB) {
        if (I.getType()->isPointerTy())
          Universe.insert(&I);
        for (auto &Op : I.operands()) {
          if (Op->getType()->isPointerTy())
            Universe.insert(Op.get());
        }
      }
    }
  }
  return NonNullDomain(Universe);
}

bool allCalleesDefined(const std::vector<llvm::Function *> &Callees) {
  if (Callees.empty())
    return false;
  for (auto *Callee : Callees) {
    if (Callee == nullptr || Callee->isDeclaration() || Callee->empty())
      return false;
  }
  return true;
}

class InterNonNullProblem final
    : public LLVMInterEliminationProblem<InterNonNullAnalysisTypes> {
public:
  InterNonNullProblem(llvm::Function *Entry, llvm::AssumptionCache *AC,
                      llvm::DominatorTree *DT,
                      const dataflow::controlflow::InterCFG *ICF)
      : LLVMInterEliminationProblem<InterNonNullAnalysisTypes>(
            std::vector<llvm::Function *>{Entry}, ICF, makeDomain(Entry)),
        Entry(Entry), AC(AC), DT(DT),
        DL(Entry != nullptr ? &Entry->getParent()->getDataLayout() : nullptr) {
    buildTransferCache(Entry != nullptr ? Entry->getParent() : nullptr);
  }

  transfer_t edgeTransfer(n_t Src, n_t Dst) const override {
    return {Src, Dst};
  }

  n_t transferNode(const transfer_t &Transfer) const override {
    return Transfer.Src;
  }

  n_t transferSuccessor(const transfer_t &Transfer) const override {
    return Transfer.Dst;
  }

  fact_t applyTransfer(const transfer_t &Transfer,
                       const fact_t &In) const override {
    fact_t Out = In;
    if (Transfer.Src == nullptr)
      return Out;
    auto It = StaticTransfers.find({Transfer.Src, Transfer.Dst});
    if (It != StaticTransfers.end())
      Out.unionWith(It->second);
    addStateDependentFacts(Transfer.Src, In, Out);
    return Out;
  }

  fact_t normalFlow(n_t Inst, const fact_t &In) override {
    return applyTransfer({Inst, nullptr}, In);
  }

  fact_t callFlow(n_t CallSite, f_t Callee, const fact_t &In) override {
    fact_t Out = In;
    auto *Call = llvm::dyn_cast_or_null<llvm::CallBase>(CallSite);
    if (Call == nullptr || Callee == nullptr)
      return Out;

    llvm_inter::forEachActualFormalPair(
        Call, Callee,
        [&](llvm::Value *Actual, llvm::Argument *Formal, unsigned Index) {
          if (!Formal->getType()->isPointerTy())
            return;
          if (In.count(Actual) ||
              Callee->hasParamAttribute(Index, llvm::Attribute::NonNull) ||
              Callee->hasParamAttribute(Index,
                                        llvm::Attribute::Dereferenceable)) {
            Out.insert(Formal);
          }
        });
    llvm_inter::copyGlobalValueFacts(In, Out);
    return Out;
  }

  fact_t returnFlow(n_t CallSite, f_t, n_t ExitStmt, n_t,
                    const fact_t &In) override {
    fact_t Out = In;
    auto *Call = llvm::dyn_cast_or_null<llvm::CallBase>(CallSite);
    auto *Ret = llvm::dyn_cast_or_null<llvm::ReturnInst>(ExitStmt);
    if (llvm_inter::hasConcreteReturnValue(Call, Ret) &&
        In.count(Ret->getReturnValue())) {
      Out.insert(CallSite);
    }
    return Out;
  }

  fact_t returnFlowWithCallerFact(n_t CallSite, f_t Callee, n_t ExitStmt,
                                  n_t RetSite, const fact_t &CalleeExit,
                                  const fact_t &CallerFact) override {
    fact_t Out = CallerFact;
    auto Returned = returnFlow(CallSite, Callee, ExitStmt, RetSite, CalleeExit);
    Out.unionWith(Returned);
    return Out;
  }

  fact_t callToRetFlow(n_t CallSite, n_t RetSite,
                       const std::vector<f_t> &Callees,
                       const fact_t &In) override {
    if (allCalleesDefined(Callees))
      return this->bottom();
    return applyTransfer({CallSite, RetSite}, In);
  }

  std::unordered_map<n_t, fact_t> initialSeeds() override {
    std::unordered_map<n_t, fact_t> Seeds;
    auto *Entry = getEntryPoints().empty() ? nullptr : getEntryPoints().front();
    if (Entry == nullptr || Entry->empty())
      return Seeds;
    auto Initial = this->getAbstractDomain().empty();
    for (auto &Arg : Entry->args()) {
      if (Arg.getType()->isPointerTy() && Arg.hasNonNullAttr())
        Initial.insert(&Arg);
    }
    Seeds[&Entry->getEntryBlock().front()] = std::move(Initial);
    return Seeds;
  }

private:
  llvm::Function *Entry = nullptr;
  llvm::AssumptionCache *AC = nullptr;
  llvm::DominatorTree *DT = nullptr;
  const llvm::DataLayout *DL = nullptr;
  std::map<std::pair<n_t, n_t>, fact_t> StaticTransfers;

  static const llvm::Value *getNullCheckedPointer(const llvm::ICmpInst *Cmp) {
    if (Cmp == nullptr || !Cmp->isEquality())
      return nullptr;
    if (llvm::isa<llvm::ConstantPointerNull>(Cmp->getOperand(0)))
      return Cmp->getOperand(1);
    if (llvm::isa<llvm::ConstantPointerNull>(Cmp->getOperand(1)))
      return Cmp->getOperand(0);
    return nullptr;
  }

  static void addStateDependentFacts(n_t Inst, const fact_t &In, fact_t &Out) {
    if (Inst == nullptr || !Inst->getType()->isPointerTy())
      return;
    if (auto *Cast = llvm::dyn_cast<llvm::CastInst>(Inst)) {
      if (In.count(Cast->getOperand(0)))
        Out.insert(Cast);
    } else if (auto *GEP = llvm::dyn_cast<llvm::GetElementPtrInst>(Inst)) {
      if (In.count(GEP->getPointerOperand()))
        Out.insert(GEP);
    } else if (auto *Select = llvm::dyn_cast<llvm::SelectInst>(Inst)) {
      if (In.count(Select->getTrueValue()) &&
          In.count(Select->getFalseValue())) {
        Out.insert(Select);
      }
    } else if (auto *Phi = llvm::dyn_cast<llvm::PHINode>(Inst)) {
      bool AllNonNull = Phi->getNumIncomingValues() != 0;
      for (auto &Incoming : Phi->incoming_values())
        AllNonNull &= In.count(Incoming.get()) != 0;
      if (AllNonNull)
        Out.insert(Phi);
    }
  }

  bool isKnownNonNull(const llvm::Value *V,
                      const llvm::Instruction *Ctx) const {
    if (V == nullptr || !V->getType()->isPointerTy() || DL == nullptr)
      return false;
    const bool IsEntryFunction = Ctx != nullptr && Ctx->getFunction() == Entry;
    return llvm::isKnownNonZero(V, *DL, 0, IsEntryFunction ? AC : nullptr, Ctx,
                                IsEntryFunction ? DT : nullptr);
  }

  void addInstructionFacts(n_t Inst, fact_t &Out) const {
    if (Inst == nullptr)
      return;
    if (llvm::isa<llvm::AllocaInst>(Inst) || isKnownNonNull(Inst, Inst))
      Out.insert(Inst);
    if (auto *Call = llvm::dyn_cast<llvm::CallBase>(Inst)) {
      if (Call->getType()->isPointerTy() &&
          (Call->hasRetAttr(llvm::Attribute::NonNull) ||
           Call->hasRetAttr(llvm::Attribute::Dereferenceable))) {
        Out.insert(Call);
      }
      auto *Callee = Call->getCalledFunction();
      if (Callee != nullptr) {
        unsigned Index = 0;
        for (auto &Arg : Call->args()) {
          if (Arg->getType()->isPointerTy() &&
              (Callee->hasParamAttribute(Index, llvm::Attribute::NonNull) ||
               Callee->hasParamAttribute(Index,
                                         llvm::Attribute::Dereferenceable))) {
            Out.insert(Arg.get());
          }
          ++Index;
        }
      }
      if (Call->getIntrinsicID() == llvm::Intrinsic::assume &&
          !Call->arg_empty()) {
        auto *Cmp = llvm::dyn_cast<llvm::ICmpInst>(Call->getArgOperand(0));
        auto *Ptr = getNullCheckedPointer(Cmp);
        if (Ptr != nullptr && Cmp->getPredicate() == llvm::ICmpInst::ICMP_NE)
          Out.insert(Ptr);
      }
    }
  }

  void addBranchFact(n_t Src, n_t Dst, fact_t &Out) const {
    auto *Branch = llvm::dyn_cast_or_null<llvm::BranchInst>(Src);
    if (Branch == nullptr || !Branch->isConditional())
      return;
    auto *Cmp = llvm::dyn_cast<llvm::ICmpInst>(Branch->getCondition());
    auto *Ptr = getNullCheckedPointer(Cmp);
    if (Ptr == nullptr)
      return;
    const bool TrueEdge = Branch->getSuccessor(0) ==
                          (Dst != nullptr ? Dst->getParent() : nullptr);
    const bool NonNullEdge =
        Cmp->getPredicate() == llvm::ICmpInst::ICMP_NE ? TrueEdge : !TrueEdge;
    if (NonNullEdge)
      Out.insert(Ptr);
  }

  void buildTransferCache(llvm::Module *M) {
    if (M == nullptr)
      return;
    const auto Empty = this->getAbstractDomain().empty();
    for (auto &F : *M) {
      for (auto &BB : F) {
        for (auto &I : BB) {
          auto Succs = getICFG()->getSuccsOf(
              &I, dataflow::controlflow::FlowDirection::Forward);
          if (Succs.empty())
            Succs.push_back(nullptr);
          for (auto *Dst : Succs) {
            auto Facts = Empty;
            addInstructionFacts(&I, Facts);
            addBranchFact(&I, Dst, Facts);
            StaticTransfers[{&I, Dst}] = std::move(Facts);
          }
        }
      }
    }
  }
};

} // namespace

InterNonNullResult
runInterElimNonNull(llvm::Function *Entry, llvm::AssumptionCache *AC,
                    llvm::DominatorTree *DT,
                    const dataflow::controlflow::InterCFG *ICF, EliminationOptions Options) {
  InterNonNullResult Out;
  if (Entry == nullptr || Entry->isDeclaration())
    return Out;
  std::unique_ptr<dataflow::controlflow::LLVMInterCFG> OwnedICF;
  if (ICF == nullptr) {
    OwnedICF = std::make_unique<dataflow::controlflow::LLVMInterCFG>(
        Entry->getParent());
    ICF = OwnedICF.get();
  }
  InterNonNullProblem Problem(Entry, AC, DT, ICF);
  InterEliminationSolver<InterNonNullAnalysisTypes,
                         kDefaultInterElimNonNullCallStringLength>
      Solver(Problem, Options);
  auto Status = Solver.solve();
  if (const auto *Result = Solver.getResults())
    Out = *Result;
  Out.setSolveStatus(Status);
  return Out;
}

InterNonNullResult
runInterSummaryElimNonNull(llvm::Function *Entry, llvm::AssumptionCache *AC,
                           llvm::DominatorTree *DT,
                           const dataflow::controlflow::InterCFG *ICF,
                           PathSummaryEquationOptions Options) {
  InterNonNullResult Out;
  if (Entry == nullptr || Entry->isDeclaration())
    return Out;
  std::unique_ptr<dataflow::controlflow::LLVMInterCFG> OwnedICF;
  if (ICF == nullptr) {
    OwnedICF = std::make_unique<dataflow::controlflow::LLVMInterCFG>(
        Entry->getParent());
    ICF = OwnedICF.get();
  }
  InterNonNullProblem Problem(Entry, AC, DT, ICF);
  ForwardInterSummarySolver<InterNonNullAnalysisTypes,
                            kDefaultInterElimNonNullCallStringLength>
      Solver(Problem, Options);
  auto Status = Solver.solve();
  if (const auto *Result = Solver.getResults())
    Out = *Result;
  Out.setSolveStatus(Status);
  Out.setSummarySolveDiagnostics(Solver.resultDiagnostics());
  return Out;
}

} // namespace elimination
