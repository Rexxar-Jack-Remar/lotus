#include "Dataflow/Elimination/Analyses/Intraprocedural/EliminationLiveVariables.h"

#include "Dataflow/ControlFlow/IntraCFG.h"
#include "Dataflow/Elimination/EliminationFramework.h"
#include "Dataflow/Elimination/LLVM/LLVMReverseEliminationProblem.h"
#include "Dataflow/Elimination/Solver/IntraEliminationSolver.h"

#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"

#include <unordered_set>

namespace elimination {
namespace {

using LiveVariablesDomain = LLVMEliminationDomain<LiveVariablesFact>;

class ReverseLiveVariablesProblem
    : public LLVMReverseIntraEliminationProblem<LiveVariablesFact> {
public:
  explicit ReverseLiveVariablesProblem(llvm::Function *F,
                                       llvm::Instruction *Exit)
      : LLVMReverseIntraEliminationProblem<LiveVariablesFact>(F, Exit) {}

  fact_t applyTransfer(const transfer_t &T, const fact_t &In) const override {
    auto *Inst = T;
    fact_t Out = In;
    if (Inst == nullptr) {
      return Out;
    }

    if (llvm::isa<llvm::DbgInfoIntrinsic>(Inst)) {
      return Out;
    }

    if (!Inst->getType()->isVoidTy()) {
      Out.erase(Inst);
    }

    for (auto &Op : Inst->operands()) {
      auto *V = Op.get();
      if (llvm::isa<llvm::Instruction>(V) || llvm::isa<llvm::Argument>(V)) {
        Out.insert(V);
      }
    }

    return Out;
  }

  fact_t meet(const fact_t &Lhs, const fact_t &Rhs) const override {
    fact_t Out = Lhs;
    Out.insert(Rhs.begin(), Rhs.end());
    return Out;
  }

  bool equal_to(const fact_t &Lhs, const fact_t &Rhs) const override {
    return Lhs == Rhs;
  }

  fact_t meetIdentity() const override { return fact_t{}; }

  fact_t initialFact() const override { return fact_t{}; }
};

std::vector<llvm::Instruction *> getExitInstructions(llvm::Function *F) {
  std::vector<llvm::Instruction *> Exits;
  if (F == nullptr || F->isDeclaration()) {
    return Exits;
  }
  dataflow::controlflow::LLVMIntraCFG CFG;
  for (auto &BB : *F) {
    for (auto &I : BB) {
      if (CFG.getSuccsOf(&I, dataflow::controlflow::FlowDirection::Forward).empty()) {
        Exits.push_back(&I);
      }
    }
  }
  return Exits;
}

} // namespace

LiveVariablesResult runIntraElimLiveVariables(llvm::Function *F,
                                              EliminationOptions Opts) {
  LiveVariablesResult Combined;
  if (F == nullptr || F->isDeclaration()) {
    return Combined;
  }

  auto Exits = getExitInstructions(F);
  if (Exits.empty()) {
    return Combined;
  }

  for (auto *Exit : Exits) {
    ReverseLiveVariablesProblem Problem(F, Exit);
    IntraEliminationSolver<LiveVariablesDomain> Solver(Problem, Opts);
    Solver.solve();
    auto Res = Solver.getResults();

    auto ExitFacts = Res.IN(Exit);
    Res.IN(Exit) = Problem.applyTransfer(Exit, ExitFacts);

    for (auto &BB : *F) {
      for (auto &I : BB) {
        auto *Inst = &I;
        auto &Out = Combined.IN(Inst);
        const auto &InFacts = Res.IN(Inst);
        Out.insert(InFacts.begin(), InFacts.end());
      }
    }
  }

  return Combined;
}

} // namespace elimination
