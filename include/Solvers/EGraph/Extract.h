#pragma once

#include "Solvers/EGraph/EGraph.h"

namespace lotus::egraph {

template <typename L> struct AstSize {
  using Cost = size_t;

  template <typename F> Cost operator()(const L &node, F &&child_cost) const {
    size_t total = 1;
    for (Id child : node.children()) {
      total = total + child_cost(child);
    }
    return total;
  }
};

template <typename L> struct AstDepth {
  using Cost = size_t;

  template <typename F> Cost operator()(const L &node, F &&child_cost) const {
    size_t depth = 1;
    for (Id child : node.children()) {
      depth = std::max(depth, size_t(1) + child_cost(child));
    }
    return depth;
  }
};

template <typename L, typename A = NoAnalysis<L>, typename CostFn = AstSize<L>>
class Extractor {
public:
  using Cost = typename CostFn::Cost;

  Extractor(const EGraph<L, A> &egraph, CostFn cost_fn = CostFn())
      : egraph_(egraph), cost_fn_(std::move(cost_fn)) {
    compute();
  }

  std::pair<Cost, RecExpr<L>> findBest(Id eclass) const {
    Id canonical = egraph_.find(eclass);
    const auto &entry = best_.at(canonical);
    return {entry.cost, buildExpr(canonical)};
  }

  const L &findBestNode(Id eclass) const {
    return best_.at(egraph_.find(eclass)).node;
  }

  Cost findBestCost(Id eclass) const { return best_.at(egraph_.find(eclass)).cost; }

private:
  struct Entry {
    Cost cost{};
    L node;
    bool valid = false;
  };

  void compute() {
    bool changed = true;
    while (changed) {
      changed = false;
      for (Id id : egraph_.classIds()) {
        const auto &klass = egraph_[id];
        Entry best_entry;
        for (const auto &node : klass.nodes) {
          bool all_children_known = true;
          auto child_cost = [&](Id child) -> Cost {
            auto it = best_.find(egraph_.find(child));
            if (it == best_.end() || !it->second.valid) {
              all_children_known = false;
              return Cost{};
            }
            return it->second.cost;
          };

          Cost cost = cost_fn_(node, child_cost);
          if (!all_children_known) {
            continue;
          }
          if (!best_entry.valid || cost < best_entry.cost) {
            best_entry = Entry{cost, node, true};
          }
        }

        auto &slot = best_[id];
        if (best_entry.valid &&
            (!slot.valid || best_entry.cost < slot.cost || best_entry.node != slot.node)) {
          slot = best_entry;
          changed = true;
        }
      }
    }
  }

  RecExpr<L> buildExpr(Id eclass) const {
    const auto &entry = best_.at(egraph_.find(eclass));
    RecExpr<L> expr;
    buildInto(expr, entry.node);
    return expr;
  }

  Id buildInto(RecExpr<L> &expr, const L &node) const {
    auto materialized = node.mapChildren([&](Id child) {
      return buildInto(expr, best_.at(egraph_.find(child)).node);
    });
    return expr.add(materialized);
  }

  const EGraph<L, A> &egraph_;
  mutable CostFn cost_fn_;
  std::unordered_map<Id, Entry> best_;
};

} // namespace lotus::egraph
