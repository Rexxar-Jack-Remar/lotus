#include "Dataflow/Mono/Analyses/Intraprocedural/IntraMonoSolverTest.h"

#include "Dataflow/Mono/DataFlowResult.h"
#include "Dataflow/Mono/IntraMonoProblem.h"

#include "llvm/IR/Instructions.h"

using namespace llvm;

namespace mono {
namespace {

struct TestDomain : LLVMMonoAnalysisDomain<std::set<Value *>> {};

class IntraSolverTestProblem : public IntraMonoProblem<TestDomain> {
public:
  explicit IntraSolverTestProblem(Function *F)
      : IntraMonoProblem<TestDomain>(std::vector<Function *>{F}) {}

  mono_container_t normalFlow(Instruction *Inst, const mono_container_t &In) override {
    mono_container_t Out = In;

    // Add the instruction result if it produces a value.
    if (Inst != nullptr && !Inst->getType()->isVoidTy()) {
      Out.insert(Inst);
    }
    // Add all used operands as "facts" to exercise GEN behavior.
    for (auto &Op : Inst->operands()) {
      if (auto *V = Op.get()) {
        Out.insert(V);
      }
    }
    return Out;
  }

  mono_container_t merge(const mono_container_t &Lhs,
                         const mono_container_t &Rhs) override {
    mono_container_t Out = Lhs;
    Out.insert(Rhs.begin(), Rhs.end());
    return Out;
  }

  bool equal_to(const mono_container_t &Lhs, const mono_container_t &Rhs) override {
    return Lhs == Rhs;
  }

  std::unordered_map<Instruction *, mono_container_t> initialSeeds() override {
    std::unordered_map<Instruction *, mono_container_t> Seeds;
    Function *F = this->getEntryPoints().empty() ? nullptr : this->getEntryPoints().front();
    if (F == nullptr || F->empty()) {
      return Seeds;
    }
    Seeds[&F->getEntryBlock().front()] = mono_container_t{};
    return Seeds;
  }
};

} // namespace

std::unique_ptr<DataFlowResult> runIntraMonoSolverTest(Function *F) {
  if (F == nullptr || F->isDeclaration()) {
    return {};
  }

  IntraSolverTestProblem Problem(F);
  IntraMonoSolver<TestDomain> Solver(Problem);
  Solver.solve();

  auto Result = std::make_unique<DataFlowResult>();
  for (auto &BB : *F) {
    for (auto &I : BB) {
      Result->IN(&I) = Solver.getInResultsAt(&I);
      Result->OUT(&I) = Solver.getOutResultsAt(&I);
    }
  }
  return Result;
}

} // namespace mono

