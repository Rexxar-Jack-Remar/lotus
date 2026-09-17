#include "Dataflow/APA/Analyses/Inter/AvailableExpressions.h"

#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"

#include "Dataflow/APA/LLVM/InterProblem.h"
#include "Dataflow/APA/Solver/Inter/ExpandedSolver.h"

#include <memory>
#include <set>
#include <unordered_map>

namespace elimination {
namespace {

struct InterAvailableExpressionsAnalysisTypes {
  using n_t = llvm::Instruction *;
  using fact_t = AvailableExpressionsFact;
  using transfer_t = llvm::Instruction *;
  using f_t = llvm::Function *;
  using i_t = dataflow::controlflow::InterCFG;
  using abstract_domain_t = AvailableExpressionsDomain;
};

bool isCandidateExpression(const llvm::Instruction *Inst) {
  if (Inst == nullptr || Inst->getType()->isVoidTy() ||
      llvm::isa<llvm::PHINode>(Inst) || llvm::isa<llvm::AllocaInst>(Inst) ||
      Inst->isTerminator()) {
    return false;
  }
  if (auto *Load = llvm::dyn_cast<llvm::LoadInst>(Inst))
    return !Load->isVolatile();
  return !Inst->mayHaveSideEffects() &&
         llvm::isSafeToSpeculativelyExecute(Inst);
}

AvailableExpressionsDomain makeDomain(llvm::Function *Entry) {
  AvailableExpressionsFact Universe;
  auto *M = Entry != nullptr ? Entry->getParent() : nullptr;
  if (M != nullptr) {
    for (auto &F : *M) {
      for (auto &BB : F) {
        for (auto &I : BB) {
          if (isCandidateExpression(&I))
            Universe.insert(makeExpressionKey(&I));
        }
      }
    }
  }
  return AvailableExpressionsDomain(Universe);
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

class InterAvailableExpressionsProblem final
    : public LLVMInterEliminationProblem<
          InterAvailableExpressionsAnalysisTypes> {
public:
  InterAvailableExpressionsProblem(llvm::Function *Entry,
                                   const dataflow::controlflow::InterCFG *ICF)
      : LLVMInterEliminationProblem<InterAvailableExpressionsAnalysisTypes>(
            std::vector<llvm::Function *>{Entry}, ICF, makeDomain(Entry)) {
    buildTransferCache(Entry != nullptr ? Entry->getParent() : nullptr);
  }

  fact_t normalFlow(n_t Inst, const fact_t &In) override {
    fact_t Out = In;
    auto It = Transfers.find(Inst);
    if (It == Transfers.end())
      return Out;
    Out.subtract(It->second.Kill);
    Out.unionWith(It->second.Gen);
    return Out;
  }

  fact_t callFlow(n_t, f_t, const fact_t &In) override { return In; }

  fact_t returnFlow(n_t CallSite, f_t, n_t, n_t, const fact_t &In) override {
    return normalFlow(CallSite, In);
  }

  fact_t returnFlowWithCallerFact(n_t CallSite, f_t, n_t, n_t,
                                  const fact_t &CalleeExit,
                                  const fact_t &) override {
    return normalFlow(CallSite, CalleeExit);
  }

  fact_t callToRetFlow(n_t CallSite, n_t, const std::vector<f_t> &Callees,
                       const fact_t &In) override {
    if (allCalleesDefined(Callees))
      return this->bottom();
    return normalFlow(CallSite, In);
  }

  std::unordered_map<n_t, fact_t> initialSeeds() override {
    std::unordered_map<n_t, fact_t> Seeds;
    auto *Entry = getEntryPoints().empty() ? nullptr : getEntryPoints().front();
    if (Entry != nullptr && !Entry->empty()) {
      Seeds[&Entry->getEntryBlock().front()] =
          this->getAbstractDomain().empty();
    }
    return Seeds;
  }

private:
  struct TransferInfo {
    fact_t Gen;
    fact_t Kill;
  };

  std::set<ExpressionKey> LoadExpressions;
  std::unordered_map<n_t, TransferInfo> Transfers;

  void killLoads(fact_t &Fact) const {
    for (auto It = Fact.begin(); It != Fact.end();) {
      if (LoadExpressions.count(*It) != 0)
        It = Fact.erase(It);
      else
        ++It;
    }
  }

  void buildTransferCache(llvm::Module *M) {
    if (M == nullptr)
      return;
    auto Universe = this->bottom();
    for (const auto &Expression : Universe) {
      if (isLoadKey(Expression))
        LoadExpressions.insert(Expression);
    }
    const auto Empty = this->getAbstractDomain().empty();
    for (auto &F : *M) {
      for (auto &BB : F) {
        for (auto &I : BB) {
          auto &Transfer = Transfers[&I];
          Transfer.Gen = Empty;
          Transfer.Kill = Empty;
          const bool KillsMemory =
              (llvm::isa<llvm::LoadInst>(I) &&
               llvm::cast<llvm::LoadInst>(I).isVolatile()) ||
              I.mayWriteToMemory();
          if (KillsMemory) {
            auto Survivors = Universe;
            killLoads(Survivors);
            Transfer.Kill = Universe;
            Transfer.Kill.subtract(Survivors);
          }
          if (isCandidateExpression(&I))
            Transfer.Gen.insert(makeExpressionKey(&I));
        }
      }
    }
  }
};

} // namespace

InterAvailableExpressionsResult
runInterElimAvailableExpressions(llvm::Function *Entry,
                                 const dataflow::controlflow::InterCFG *ICF, EliminationOptions Options) {
  InterAvailableExpressionsResult Out;
  if (Entry == nullptr || Entry->isDeclaration())
    return Out;
  std::unique_ptr<dataflow::controlflow::LLVMInterCFG> OwnedICF;
  if (ICF == nullptr) {
    OwnedICF = std::make_unique<dataflow::controlflow::LLVMInterCFG>(
        Entry->getParent());
    ICF = OwnedICF.get();
  }
  InterAvailableExpressionsProblem Problem(Entry, ICF);
  InterEliminationSolver<InterAvailableExpressionsAnalysisTypes,
                         kDefaultInterElimAvailableExpressionsCallStringLength>
      Solver(Problem, Options);
  auto Status = Solver.solve();
  if (const auto *Result = Solver.getResults())
    Out = *Result;
  Out.setSolveStatus(Status);
  return Out;
}

InterAvailableExpressionsResult runInterSummaryElimAvailableExpressions(
    llvm::Function *Entry, const dataflow::controlflow::InterCFG *ICF,
    PathSummaryEquationOptions Options) {
  InterAvailableExpressionsResult Out;
  if (Entry == nullptr || Entry->isDeclaration())
    return Out;
  std::unique_ptr<dataflow::controlflow::LLVMInterCFG> OwnedICF;
  if (ICF == nullptr) {
    OwnedICF = std::make_unique<dataflow::controlflow::LLVMInterCFG>(
        Entry->getParent());
    ICF = OwnedICF.get();
  }
  InterAvailableExpressionsProblem Problem(Entry, ICF);
  ForwardInterSummarySolver<
      InterAvailableExpressionsAnalysisTypes,
      kDefaultInterElimAvailableExpressionsCallStringLength>
      Solver(Problem, Options);
  auto Status = Solver.solve();
  if (const auto *Result = Solver.getResults())
    Out = *Result;
  Out.setSolveStatus(Status);
  Out.setSummarySolveDiagnostics(Solver.resultDiagnostics());
  return Out;
}

} // namespace elimination
