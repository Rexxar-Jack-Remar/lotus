#pragma once

#include "Solvers/EGraph/Extract.h"

#include <type_traits>

namespace lotus::egraph {

template <typename L, typename A = NoAnalysis<L>> struct LpCostFunction {
  double nodeCost(const EGraph<L, A> &, Id, const L &) { return 1.0; }
};

template <typename L, typename A = NoAnalysis<L>,
          typename CostFn = LpCostFunction<L, A>>
class LpExtractor {
public:
  explicit LpExtractor(const EGraph<L, A> &egraph, CostFn cost_fn = CostFn())
      : egraph_(egraph), cost_fn_(std::move(cost_fn)) {}

  RecExpr<L> solve(Id root) { return solveMultiple({root}).first; }

  template <typename Solver> RecExpr<L> solveWith(Id root, Solver) {
    return solve(root);
  }

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
    auto order = topologicalOrder(canonical_roots);
    std::unordered_set<Id> required(order.begin(), order.end());
    std::unordered_map<Id, Id> expr_cache;
    RecExpr<L> expr;
    for (Id root : canonical_roots) {
      buildExpr(expr, chooseNode(root, required), expr_cache, required);
    }
    return {expr, std::move(canonical_roots)};
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
  std::vector<Id> topologicalOrder(const std::vector<Id> &roots) const {
    std::vector<Id> order;
    std::unordered_set<Id> visited;
    std::function<void(Id)> dfs = [&](Id id) {
      id = egraph_.find(id);
      if (!visited.insert(id).second) {
        return;
      }
      const auto &klass = egraph_[id];
      for (const auto &node : klass.nodes) {
        for (Id child : node.children()) {
          dfs(child);
        }
      }
      order.push_back(id);
    };
    for (Id root : roots) {
      dfs(root);
    }
    return order;
  }

  double nodeCost(Id eclass, const L &node) const {
    if constexpr (std::is_same_v<CostFn, AstSize<L>> ||
                  std::is_same_v<CostFn, AstDepth<L>>) {
      return 1.0;
    } else {
      return const_cast<CostFn &>(cost_fn_).nodeCost(egraph_, eclass, node);
    }
  }

  const L &chooseNode(Id eclass, const std::unordered_set<Id> &required) const {
    eclass = egraph_.find(eclass);
    const auto &klass = egraph_[eclass];

    const L *best = nullptr;
    double best_score = std::numeric_limits<double>::infinity();
    size_t best_gain = 0;
    for (const auto &node : klass.nodes) {
      size_t reuse_gain = 0;
      for (Id child : node.children()) {
        if (required.count(egraph_.find(child))) {
          ++reuse_gain;
        }
      }
      double score = nodeCost(eclass, node);
      if (!best || reuse_gain > best_gain ||
          (reuse_gain == best_gain && score < best_score)) {
        best = &node;
        best_score = score;
        best_gain = reuse_gain;
      }
    }
    return *best;
  }

  Id buildExpr(RecExpr<L> &expr, const L &node,
               std::unordered_map<Id, Id> &expr_cache,
               const std::unordered_set<Id> &required) const {
    auto materialized = node.mapChildren([&](Id child) {
      child = egraph_.find(child);
      auto it = expr_cache.find(child);
      if (it != expr_cache.end()) {
        return it->second;
      }
      Id built = buildExpr(expr, chooseNode(child, required), expr_cache, required);
      expr_cache.emplace(child, built);
      return built;
    });
    return expr.add(materialized);
  }

  const EGraph<L, A> &egraph_;
  mutable CostFn cost_fn_;
};

} // namespace lotus::egraph
