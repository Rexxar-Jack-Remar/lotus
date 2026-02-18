/*
 *
 * Author: rainoftime
*/
#include "Dataflow/Mono/Analyses/Intra/IntraLiveVariables.h"
#include "Dataflow/Mono/Container/Traits.h"
#include "Dataflow/Mono/Core/Domain.h"
#include "Dataflow/Mono/Core/Problem.h"
#include "Dataflow/Mono/Solver/IntraSolver.h"

using namespace llvm;

namespace mono {

namespace {

using LiveVariablesDomain = LLVMMonoAnalysisDomain<SetContainer<Value *>>;

class LiveVariablesProblem : public IntraMonoProblem<LiveVariablesDomain> {
public:
  explicit LiveVariablesProblem(Function *F)
      : IntraMonoProblem<LiveVariablesDomain>({F}) {}

  ::dataflow::controlflow::FlowDirection direction() const override {
    return ::dataflow::controlflow::FlowDirection::Backward;
  }

  mono_container_t normalFlow(Instruction *Inst,
                               const mono_container_t &In) override {
    mono_container_t Out = In;

    if (!Inst->getType()->isVoidTy()) {
      Out.erase(Inst);
    }

    for (auto &Op : Inst->operands()) {
      if (isa<Instruction>(Op) || isa<Argument>(Op)) {
        Out.insert(Op);
      }
    }

    return Out;
  }

  mono_container_t merge(const mono_container_t &Lhs,
                          const mono_container_t &Rhs) override {
    mono_container_t Out = Lhs;
    Out.unionWith(Rhs);
    return Out;
  }

  bool equal_to(const mono_container_t &Lhs,
                const mono_container_t &Rhs) override {
    return Lhs == Rhs;
  }

  std::unordered_map<Instruction *, mono_container_t> initialSeeds() override {
    std::unordered_map<Instruction *, mono_container_t> Seeds;
    auto *F = getEntryPoints().empty() ? nullptr : getEntryPoints().front();
    if (F == nullptr) {
      return Seeds;
    }
    for (auto &BB : *F) {
      if (auto *Ret = dyn_cast<ReturnInst>(BB.getTerminator())) {
        Seeds[Ret] = {};
      }
    }
    return Seeds;
  }
};

} // namespace

// SSA register liveness analysis
std::unique_ptr<DataFlowResult> runLiveVariablesAnalysis(Function *f) {
  if (f == nullptr || f->isDeclaration()) {
    return nullptr;
  }

  LiveVariablesProblem Problem(f);
  IntraMonoSolver<LiveVariablesDomain> Solver(Problem);
  Solver.solve();

  // B6 fix: for a backward analysis the solver's AnalysisIn[n] holds the
  // facts that flow *into* the backward traversal at node n.  In conventional
  // (forward-view) liveness terminology:
  //
  //   Backward solver AnalysisIn[n]  = values live BEFORE n  = forward IN[n]
  //   Backward solver AnalysisOut[n] = values live AFTER  n  = forward OUT[n]
  //
  // The previous code had these two assignments swapped, so callers querying
  // Result->IN(I) received the post-instruction live set and vice-versa.
  auto Result = std::make_unique<DataFlowResult>();
  for (auto &BB : *f) {
    for (auto &Inst : BB) {
      auto *I = &Inst;
      // IN[n]  = values live before n  (backward solver's AnalysisIn)
      Result->IN(I)  = Solver.getInResultsAt(I).getSet();
      // OUT[n] = values live after  n  (backward solver's AnalysisOut)
      Result->OUT(I) = Solver.getOutResultsAt(I).getSet();
      for (auto &Op : I->operands()) {
        if (isa<Instruction>(Op) || isa<Argument>(Op)) {
          Result->GEN(I).insert(Op);
        }
      }
      if (!I->getType()->isVoidTy()) {
        Result->KILL(I).insert(I);
      }
    }
  }

  return Result;
}

} // namespace mono
