#include "Dataflow/Elimination/Analyses/Intraprocedural/EliminationLiveVariables.h"

#include "Dataflow/Elimination/EliminationFramework.h"
#include "Dataflow/Elimination/Solver/IntraEliminationSolver.h"
#include "Dataflow/Mono/ControlFlow/IntraCFG.h"

#include "llvm/IR/CFG.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"

#include <unordered_set>

namespace elimination {
namespace {

struct LiveVariablesDomain {
  using n_t = llvm::Instruction *;
  using fact_t = LiveVariablesFact;
  using transfer_t = llvm::Instruction *;
};

class ReverseLiveVariablesProblem
    : public IntraEliminationProblem<LiveVariablesDomain> {
public:
  explicit ReverseLiveVariablesProblem(llvm::Function *F, llvm::Instruction *Entry)
      : F(F), Entry(Entry) {}

  std::vector<n_t> nodes() const override {
    ensurePrepared();
    return Nodes;
  }

  n_t entry() const override { return Entry; }

  std::vector<n_t> succs(n_t Node) const override {
    return CFG.getSuccsOf(Node, mono::FlowDirection::Backward);
  }

  transfer_t edgeTransfer(n_t /*Src*/, n_t Dst) const override { return Dst; }

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

private:
  void ensurePrepared() const {
    if (Prepared) {
      return;
    }
    Prepared = true;
    Nodes.clear();
    if (F == nullptr || F->isDeclaration() || Entry == nullptr) {
      return;
    }

    std::unordered_set<n_t> Reach;
    std::vector<n_t> Stack;
    Stack.push_back(Entry);
    Reach.insert(Entry);
    while (!Stack.empty()) {
      auto *Cur = Stack.back();
      Stack.pop_back();
      for (auto *Succ : CFG.getSuccsOf(Cur, mono::FlowDirection::Backward)) {
        if (Reach.insert(Succ).second) {
          Stack.push_back(Succ);
        }
      }
    }

    for (auto *N : CFG.getAllInstructionsOf(F)) {
      if (Reach.count(N)) {
        Nodes.push_back(N);
      }
    }
  }

  llvm::Function *F = nullptr;
  llvm::Instruction *Entry = nullptr;
  mono::LLVMIntraCFG CFG;
  mutable bool Prepared = false;
  mutable std::vector<n_t> Nodes;
};

std::vector<llvm::Instruction *> getExitInstructions(llvm::Function *F) {
  std::vector<llvm::Instruction *> Exits;
  if (F == nullptr || F->isDeclaration()) {
    return Exits;
  }
  mono::LLVMIntraCFG CFG;
  for (auto &BB : *F) {
    for (auto &I : BB) {
      if (CFG.getSuccsOf(&I, mono::FlowDirection::Forward).empty()) {
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
