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

  auto Result = std::make_unique<DataFlowResult>();
  for (auto &BB : *f) {
    for (auto &Inst : BB) {
      auto *I = &Inst;
      Result->OUT(I) = Solver.getInResultsAt(I).getSet();
      Result->IN(I) = Solver.getOutResultsAt(I).getSet();
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
