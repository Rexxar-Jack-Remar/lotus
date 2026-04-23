#pragma once

#include "Solvers/EGraph/Extract.h"

namespace lotus::egraph {

template <typename L, typename A = NoAnalysis<L>> struct LpCostFunction {
  double nodeCost(const EGraph<L, A> &, Id, const L &) { return 1.0; }
};

template <typename L, typename A = NoAnalysis<L>, typename CostFn = AstSize<L>>
class LpExtractor {
public:
  explicit LpExtractor(const EGraph<L, A> &egraph, CostFn cost_fn = CostFn())
      : egraph_(egraph), extractor_(egraph, std::move(cost_fn)) {}

  RecExpr<L> solve(Id root) { return extractor_.findBest(root).second; }

  template <typename Solver> RecExpr<L> solveWith(Id root, Solver) { return solve(root); }

  template <typename Solver>
  RecExpr<L> solveWithTimeout(Id root, Solver, double timeout_seconds) {
    if (timeout_seconds < 0.0) {
      throw std::runtime_error("LP solver timeout must be non-negative");
    }
    return solve(root);
  }

  std::pair<RecExpr<L>, std::vector<Id>> solveMultiple(const std::vector<Id> &roots) {
    if (roots.empty()) {
      return {RecExpr<L>(), {}};
    }
    std::vector<Id> canonical_roots;
    canonical_roots.reserve(roots.size());
    for (Id root : roots) {
      canonical_roots.push_back(egraph_.find(root));
    }
    return {solve(roots.front()), std::move(canonical_roots)};
  }

  template <typename Solver>
  std::pair<RecExpr<L>, std::vector<Id>> solveMultipleWith(const std::vector<Id> &roots, Solver) {
    return solveMultiple(roots);
  }

  template <typename Solver>
  std::pair<RecExpr<L>, std::vector<Id>>
  solveMultipleWithTimeout(const std::vector<Id> &roots, Solver, double timeout_seconds) {
    if (timeout_seconds < 0.0) {
      throw std::runtime_error("LP solver timeout must be non-negative");
    }
    return solveMultiple(roots);
  }

private:
  const EGraph<L, A> &egraph_;
  Extractor<L, A, CostFn> extractor_;
};

} // namespace lotus::egraph
