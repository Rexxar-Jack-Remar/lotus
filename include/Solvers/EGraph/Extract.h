#pragma once

#include "Solvers/EGraph/EGraph.h"

#include <functional>
#include <limits>
#include <optional>
#include <type_traits>

namespace lotus::egraph {

namespace detail {

template <typename...> using void_t = void;

template <typename CF, typename L, typename = void>
struct HasCostMethod : std::false_type {};

template <typename CF, typename L>
struct HasCostMethod<
    CF, L,
    void_t<decltype(std::declval<CF &>().cost(
        std::declval<const L &>(),
        std::function<typename CF::Cost(Id)>{}))>> : std::true_type {};

template <typename CF, typename L, typename = void>
struct HasCallOperator : std::false_type {};

template <typename CF, typename L>
struct HasCallOperator<
    CF, L,
    void_t<decltype(std::declval<CF &>()(
        std::declval<const L &>(),
        std::function<typename CF::Cost(Id)>{}))>> : std::true_type {};

template <typename CF, typename L, typename ChildCostFn>
auto invokeCost(CF &cost_fn, const L &node, ChildCostFn &&child_cost)
    -> typename CF::Cost {
  if constexpr (HasCostMethod<CF, L>::value) {
    return cost_fn.cost(node, std::forward<ChildCostFn>(child_cost));
  } else {
    static_assert(HasCallOperator<CF, L>::value,
                  "Cost function must define Cost and either cost(node, child_cost) "
                  "or operator()(node, child_cost)");
    return cost_fn(node, std::forward<ChildCostFn>(child_cost));
  }
}

template <typename T>
T saturatingAdd(T lhs, T rhs) {
  static_assert(std::is_unsigned<T>::value,
                "saturatingAdd only supports unsigned integral types");
  constexpr T limit = std::numeric_limits<T>::max();
  return rhs > limit - lhs ? limit : static_cast<T>(lhs + rhs);
}

} // namespace detail

template <typename Derived, typename L, typename CostT> struct CostFunction {
  using Cost = CostT;

  Cost costRec(const RecExpr<L> &expr) {
    std::unordered_map<Id, Cost> costs;
    costs.reserve(expr.size());
    auto &derived = static_cast<Derived &>(*this);
    for (size_t i = 0; i < expr.size(); ++i) {
      Id id = Id::fromIndex(i);
      costs.emplace(id, derived.cost(expr[id], [&](Id child) { return costs.at(child); }));
    }
    return costs.at(expr.root());
  }
};

template <typename L> struct AstSize : CostFunction<AstSize<L>, L, size_t> {
  using Cost = typename CostFunction<AstSize<L>, L, size_t>::Cost;

  template <typename C> Cost cost(const L &node, C &&child_cost) {
    size_t total = 1;
    for (Id child : node.children()) {
      total = detail::saturatingAdd(total, child_cost(child));
    }
    return total;
  }
};

template <typename L> struct AstDepth : CostFunction<AstDepth<L>, L, size_t> {
  using Cost = typename CostFunction<AstDepth<L>, L, size_t>::Cost;

  template <typename C> Cost cost(const L &node, C &&child_cost) {
    size_t max_child_depth = 0;
    for (Id child : node.children()) {
      max_child_depth = std::max(max_child_depth, child_cost(child));
    }
    return detail::saturatingAdd<size_t>(1, max_child_depth);
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

  Cost findBestCost(Id eclass) const {
    return best_.at(egraph_.find(eclass)).cost;
  }

private:
  struct Entry {
    Cost cost{};
    L node;
  };

  void compute() {
    bool changed = true;
    while (changed) {
      changed = false;
      for (Id id : egraph_.classIds()) {
        const auto &klass = egraph_[id];
        std::optional<Entry> best_entry;
        for (const auto &node : klass.nodes) {
          auto child_cost = [&](Id child) -> std::optional<Cost> {
            auto it = best_.find(egraph_.find(child));
            if (it == best_.end()) {
              return std::nullopt;
            }
            return it->second.cost;
          };

          auto total_cost = nodeTotalCost(node, child_cost);
          if (!total_cost) {
            continue;
          }
          if (!best_entry || total_cost.value() < best_entry->cost) {
            best_entry = Entry{total_cost.value(), node};
          }
        }

        if (!best_entry) {
          continue;
        }

        auto it = best_.find(id);
        if (it == best_.end() || best_entry->cost < it->second.cost) {
          best_[id] = *best_entry;
          changed = true;
        }
      }
    }
  }

  template <typename ChildCostFn>
  std::optional<Cost> nodeTotalCost(const L &node, ChildCostFn &&child_cost) {
    std::unordered_map<Id, Cost> cached_costs;
    cached_costs.reserve(node.children().size());
    for (Id child : node.children()) {
      auto cost = child_cost(child);
      if (!cost) {
        return std::nullopt;
      }
      cached_costs.emplace(child, *cost);
    }

    return detail::invokeCost(cost_fn_, node,
                              [&](Id child) { return cached_costs.at(child); });
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
