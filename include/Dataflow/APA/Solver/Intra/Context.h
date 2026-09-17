#ifndef DATAFLOW_APA_ENGINES_SOLVERCONTEXT_H_
#define DATAFLOW_APA_ENGINES_SOLVERCONTEXT_H_

#include "Dataflow/APA/Core/Options.h"
#include "Dataflow/APA/Core/PathExpr.h"
#include "Dataflow/APA/Core/Problem.h"
#include "Dataflow/APA/Core/Result.h"
#include "Dataflow/APA/Core/Timing.h"
#include "Dataflow/APA/Solver/Interpretation/FactInterpreter.h"
#include "Dataflow/APA/Solver/Intra/ADT/Decomposition.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace elimination {
namespace detail {

// Per-run state. Structural construction and fact interpretation belong to
// their dedicated components, not this context container.
template <typename AnalysisTypesT> class IntraEliminationSolverContext {
public:
  using ProblemTy = IntraEliminationProblem<AnalysisTypesT>;
  using ReducibleProblemTy = IntraReducibleEliminationProblem<AnalysisTypesT>;
  using n_t = typename ProblemTy::n_t;
  using fact_t = typename ProblemTy::fact_t;
  using transfer_t = typename ProblemTy::transfer_t;

  using expr_factory_t = PathExprFactory<transfer_t>;
  using expr_ref_t = typename expr_factory_t::Ref;
  using result_t = DataFlowResultT<n_t, fact_t, transfer_t>;

  using ADT = ADTDecomposition<AnalysisTypesT>;
  using Edge = typename ADT::Edge;
  using ADTNode = typename ADT::ADTNode;
  using LCATable = typename ADT::LCATable;
  using ReducibleViewProvided =
      typename ADT::ReducibilityTy::ReducibleViewProvided;
  using ComputedReducibleView =
      typename ADT::ReducibilityTy::ComputedReducibleView;

  explicit IntraEliminationSolverContext(const ProblemTy &Problem,
                                         const EliminationOptions &Opts)
      : Problem(Problem), Opts(Opts), Structure(Problem, Exprs, Diagnostics),
        Interpreter(Problem, this->Opts, Diagnostics, StarNonConvergent) {
    if (!this->Opts.Order.IsStarResultCached) {
      this->Opts.Order.IsStarResultCached =
          Interpreter.starCacheSnapshot(Problem.initialFact());
    }
  }

  std::size_t idx(const n_t &N) const {
    auto It = Index.find(N);
    assert(It != Index.end());
    return It->second;
  }

  const ProblemTy &Problem;
  EliminationOptions Opts;
  mutable bool StarNonConvergent = false;
  mutable SolveDiagnostics Diagnostics;
  expr_factory_t Exprs;
  std::vector<n_t> Nodes;
  std::unordered_map<n_t, std::size_t> Index;
  std::vector<std::vector<expr_ref_t>> Matrix;
  result_t Results;
  ADT Structure;
  FactInterpreter<ProblemTy> Interpreter;
};

} // namespace detail
} // namespace elimination

#endif // DATAFLOW_APA_ENGINES_SOLVERCONTEXT_H_
