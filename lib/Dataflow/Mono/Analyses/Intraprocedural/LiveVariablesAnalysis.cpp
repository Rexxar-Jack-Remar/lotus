/*
 *
 * Author: rainoftime
*/
#include "Dataflow/Mono/Analyses/Intraprocedural/LiveVariablesAnalysis.h"
#include "Dataflow/Mono/IntraMonoProblem.h"
#include "Dataflow/Mono/LLVMMonoAnalysisDomain.h"
#include "Dataflow/Mono/Solver/IntraMonoSolver.h"

using namespace llvm;

namespace mono {

namespace {

struct LiveVariablesDomain : LLVMMonoAnalysisDomain<std::set<Value *>> {};

class LiveVariablesProblem : public IntraMonoProblem<LiveVariablesDomain> {
public:
  explicit LiveVariablesProblem(Function *F)
      : IntraMonoProblem<LiveVariablesDomain>({F}) {}

  ::dataflow::controlflow::FlowDirection direction() const override {
    return ::dataflow::controlflow::FlowDirection::Backward;
  }

  std::set<Value *> normalFlow(Instruction *Inst,
                               const std::set<Value *> &In) override {
    std::set<Value *> Out = In;

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

  std::set<Value *> merge(const std::set<Value *> &Lhs,
                          const std::set<Value *> &Rhs) override {
    std::set<Value *> Out = Lhs;
    Out.insert(Rhs.begin(), Rhs.end());
    return Out;
  }

  bool equal_to(const std::set<Value *> &Lhs,
                const std::set<Value *> &Rhs) override {
    return Lhs == Rhs;
  }

  std::unordered_map<Instruction *, std::set<Value *>> initialSeeds() override {
    std::unordered_map<Instruction *, std::set<Value *>> Seeds;
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
      Result->OUT(I) = Solver.getInResultsAt(I);
      Result->IN(I) = Solver.getOutResultsAt(I);
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
