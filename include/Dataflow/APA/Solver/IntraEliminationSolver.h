#ifndef DATAFLOW_ELIMINATION_SOLVER_INTRAELIMINATIONSOLVER_H_
#define DATAFLOW_ELIMINATION_SOLVER_INTRAELIMINATIONSOLVER_H_

#include "Dataflow/APA/Core/Options.h"
#include "Dataflow/APA/Solver/ADTDelayedEngine.h"
#include "Dataflow/APA/Solver/ADTSimpleEngine.h"
#include "Dataflow/APA/Solver/StateEliminationEngine.h"

namespace elimination {

// Public entry point for the intraprocedural APA solver.
//
// This class intentionally stays thin: it owns the shared solver context and
// dispatches to one of the three engine implementations based on
// EliminationOptions. The heavy algorithmic logic lives in the engine headers
// so that each solver family can be read and maintained independently.
template <typename AnalysisDomainTy> class IntraEliminationSolver final {
public:
  using Context = detail::IntraEliminationSolverContext<AnalysisDomainTy>;
  using ProblemTy = typename Context::ProblemTy;
  using ReducibleProblemTy = typename Context::ReducibleProblemTy;
  using n_t = typename Context::n_t;
  using fact_t = typename Context::fact_t;
  using transfer_t = typename Context::transfer_t;
  using expr_factory_t = typename Context::expr_factory_t;
  using expr_ref_t = typename Context::expr_ref_t;
  using result_t = typename Context::result_t;

  explicit IntraEliminationSolver(const ProblemTy &Problem,
                                  EliminationOptions Opts = {})
      : Ctx(Problem), Opts(Opts) {}

  // Try the requested engine first. ADT-based methods may reject the problem if
  // reducibility assumptions do not hold; in that case we transparently fall
  // back to the generic state-elimination engine.
  void solve() {
    UsedADT = false;
    if (Opts.Method == EliminationMethod::ADTSimple) {
      if (detail::solveADTSimple(Ctx)) {
        UsedADT = true;
        return;
      }
    }
    if (Opts.Method == EliminationMethod::ADTDelayed) {
      if (detail::solveADTDelayed(Ctx)) {
        UsedADT = true;
        return;
      }
    }
    detail::solveStateElimination(Ctx);
  }

  const result_t &getResults() const { return Ctx.Results; }
  bool usedADT() const { return UsedADT; }

private:
  Context Ctx;
  EliminationOptions Opts;
  bool UsedADT = false;
};

} // namespace elimination

#endif // DATAFLOW_ELIMINATION_SOLVER_INTRAELIMINATIONSOLVER_H_
