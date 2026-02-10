#include "Dataflow/Mono/Analyses/Intraprocedural/ReachableAnalysis.h"
#include "Dataflow/Mono/IntraMonoProblem.h"
#include "Dataflow/Mono/LLVMMonoAnalysisDomain.h"
#include "Dataflow/Mono/Solver/IntraMonoSolver.h"

using namespace llvm;

namespace mono {

/**
 * Reachable Analysis - A client of the monotone dataflow framework
 *
 * This is a backward dataflow analysis that computes which instructions are
 * reachable (can be executed) forward from each program point.
 *
 * Semantics of IN[i] and OUT[i]:
 *   - OUT[i] = Set of instructions reachable AFTER executing instruction i
 *   - IN[i]  = Set of instructions reachable FROM (starting at) instruction i
 *
 * Dataflow equations:
 *   - GEN[i]  = {i} if filter(i) is true, otherwise empty
 *   - KILL[i] = {} (empty set, nothing is killed)
 *   - OUT[i]  = Union of IN[succ] for all successors succ of i
 *   - IN[i]   = GEN[i] ∪ OUT[i]
 *
 * The analysis runs backward through the CFG: information flows from successors
 * to predecessors, accumulating forward reachability information.
 *
 * Author: rainoftime
 */
std::unique_ptr<DataFlowResult> runReachableAnalysis(
    Function *f,
    const std::function<bool(Instruction *i)> &filter) {

  if (f == nullptr || f->isDeclaration()) {
    return nullptr;
  }

  struct ReachableDomain : LLVMMonoAnalysisDomain<std::set<Value *>> {};
  class ReachableProblem : public IntraMonoProblem<ReachableDomain> {
  public:
    ReachableProblem(Function *F, std::function<bool(Instruction *)> Filter)
        : IntraMonoProblem<ReachableDomain>({F}), Filter(std::move(Filter)) {}

    ::dataflow::controlflow::FlowDirection direction() const override {
      return ::dataflow::controlflow::FlowDirection::Backward;
    }

    std::set<Value *> normalFlow(Instruction *Inst,
                                 const std::set<Value *> &In) override {
      std::set<Value *> Out = In;
      if (Filter(Inst)) {
        Out.insert(Inst);
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
      return {};
    }

  private:
    std::function<bool(Instruction *)> Filter;
  };

  ReachableProblem Problem(f, filter);
  IntraMonoSolver<ReachableDomain> Solver(Problem);
  Solver.solve();

  auto Result = std::make_unique<DataFlowResult>();
  for (auto &BB : *f) {
    for (auto &Inst : BB) {
      auto *I = &Inst;
      Result->OUT(I) = Solver.getInResultsAt(I);
      Result->IN(I) = Solver.getOutResultsAt(I);
      if (filter(I)) {
        Result->GEN(I).insert(I);
      }
    }
  }

  return Result;
}

std::unique_ptr<DataFlowResult> runReachableAnalysis(Function *f) {

  /*
   * Create the function that doesn't filter out instructions.
   */
  auto noFilter = [](Instruction *) -> bool { return true; };

  /*
   * Run the analysis
   */
  return runReachableAnalysis(f, noFilter);
}

} // namespace mono
