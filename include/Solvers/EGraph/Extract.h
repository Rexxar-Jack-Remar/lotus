#pragma once

#include "Solvers/EGraph/EGraph.h"

#include <functional>
#include <limits>
#include <optional>
#include <type_traits>

#include <llvm/ADT/SmallVector.h>

namespace lotus::egraph {

namespace detail {

template <typename...> using void_t = void;

template <typename CF, typename L, typename = void>
struct HasCostMethod : std::false_type {};

template <typename CF, typename L>
struct HasCostMethod<
    CF, L,
    void_t<decltype(std::declval<CF &>().cost(
        std::declval<const L &>(), std::function<typename CF::Cost(Id)>{}))>>
    : std::true_type {};

template <typename CF, typename L, typename = void>
struct HasCallOperator : std::false_type {};

template <typename CF, typename L>
struct HasCallOperator<
    CF, L,
    void_t<decltype(std::declval<CF &>()(
        std::declval<const L &>(), std::function<typename CF::Cost(Id)>{}))>>
    : std::true_type {};

template <typename CF, typename L, typename ChildCostFn>
auto invokeCost(CF &cost_fn, const L &node, ChildCostFn &&child_cost) ->
    typename CF::Cost {
  if constexpr (HasCostMethod<CF, L>::value) {
    return cost_fn.cost(node, std::forward<ChildCostFn>(child_cost));
  } else {
    static_assert(
        HasCallOperator<CF, L>::value,
        "Cost function must define Cost and either cost(node, child_cost) "
        "or operator()(node, child_cost)");
    return cost_fn(node, std::forward<ChildCostFn>(child_cost));
  }
}

template <typename T> T saturatingAdd(T lhs, T rhs) {
  static_assert(std::is_unsigned<T>::value,
                "saturatingAdd only supports unsigned integral types");
  constexpr T limit = std::numeric_limits<T>::max();
  return rhs > limit - lhs ? limit : static_cast<T>(lhs + rhs);
}

} // namespace detail

template <typename Derived, typename L, typename CostT> struct CostFunction {
  using Cost = CostT;

  Cost costRec(const RecExpr<L> &expr) {
    std::vector<Cost> costs;
    costs.reserve(expr.size());
    auto &derived = static_cast<Derived &>(*this);
    for (size_t i = 0; i < expr.size(); ++i) {
      Id id = Id::fromIndex(i);
      costs.push_back(derived.cost(
          expr[id], [&](Id child) { return costs.at(child.index()); }));
    }
    return costs.at(expr.root().index());
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
    const auto &entry = bestEntry(canonical);
    return {entry.cost, buildExpr(canonical)};
  }

  const L &findBestNode(Id eclass) const {
    Id canonical = egraph_.find(eclass);
    return egraph_[canonical].nodes[bestEntry(canonical).node_index];
  }

  Cost findBestCost(Id eclass) const {
    return bestEntry(egraph_.find(eclass)).cost;
  }

private:
  struct Entry {
    Cost cost{};
    size_t node_index = 0;
  };

  size_t slot(Id id) const { return slots_.at(egraph_.find(id)); }

  const Entry &bestEntry(Id id) const { return best_.at(slot(id)).value(); }

  void compute() {
    slots_.reserve(egraph_.classIds().size());
    for (Id id : egraph_.classIds()) {
      slots_.emplace(id, slots_.size());
    }
    best_.resize(slots_.size());
    reach_marks_.assign(slots_.size(), 0);
    std::deque<Id> worklist(egraph_.classIds().begin(),
                            egraph_.classIds().end());
    std::vector<bool> in_queue(slots_.size(), false);
    for (Id id : egraph_.classIds()) {
      in_queue[slot(id)] = true;
    }

    while (!worklist.empty()) {
      Id id = egraph_.find(worklist.front());
      worklist.pop_front();
      in_queue[slot(id)] = false;

      const auto &klass = egraph_[id];
      std::optional<Entry> best_entry;
      for (size_t index = 0; index < klass.nodes.size(); ++index) {
        const auto &node = klass.nodes[index];
        auto child_cost = [&](Id child) -> std::optional<Cost> {
          const auto &entry = best_[slot(child)];
          if (!entry) {
            return std::nullopt;
          }
          return entry->cost;
        };

        auto total_cost = nodeTotalCost(node, child_cost);
        if (!total_cost || (best_entry && !(*total_cost < best_entry->cost))) {
          continue;
        }
        if (wouldCreateCycle(id, node)) {
          continue;
        }
        best_entry = Entry{*total_cost, index};
      }

      auto &old = best_[slot(id)];
      if (!best_entry || (old && !(best_entry->cost < old->cost))) {
        continue;
      }
      old = *best_entry;
      for (Id parent : klass.parents) {
        Id canonical_parent = egraph_.find(parent);
        size_t parent_slot = slot(canonical_parent);
        if (!in_queue[parent_slot]) {
          in_queue[parent_slot] = true;
          worklist.push_back(canonical_parent);
        }
      }
    }
  }

  template <typename ChildCostFn>
  std::optional<Cost> nodeTotalCost(const L &node, ChildCostFn &&child_cost) {
    llvm::SmallVector<std::pair<Id, Cost>, 4> child_costs;
    child_costs.reserve(node.children().size());
    for (Id child : node.children()) {
      auto cost = child_cost(child);
      if (!cost) {
        return std::nullopt;
      }
      child_costs.emplace_back(child, *cost);
    }

    return detail::invokeCost(cost_fn_, node, [&](Id child) -> Cost {
      auto it =
          std::find_if(child_costs.begin(), child_costs.end(),
                       [&](const auto &entry) { return entry.first == child; });
      if (it == child_costs.end()) {
        throw std::runtime_error("Cost function requested a non-child e-class");
      }
      return it->second;
    });
  }

  bool wouldCreateCycle(Id owner, const L &node) const {
    ++reach_generation_;
    if (reach_generation_ == 0) {
      std::fill(reach_marks_.begin(), reach_marks_.end(), 0);
      ++reach_generation_;
    }
    for (Id child : node.children()) {
      if (reaches(egraph_.find(child), owner)) {
        return true;
      }
    }
    return false;
  }

  bool reaches(Id current, Id target) const {
    current = egraph_.find(current);
    if (current == target) {
      return true;
    }
    size_t current_slot = slot(current);
    if (reach_marks_[current_slot] == reach_generation_) {
      return false;
    }
    reach_marks_[current_slot] = reach_generation_;
    const auto &best = best_[current_slot];
    if (!best) {
      return false;
    }
    const auto &node = egraph_[current].nodes[best->node_index];
    for (Id child : node.children()) {
      if (reaches(egraph_.find(child), target)) {
        return true;
      }
    }
    return false;
  }

  RecExpr<L> buildExpr(Id eclass) const {
    RecExpr<L> expr;
    std::unordered_map<Id, Id> materialized;
    materialized.reserve(slots_.size());
    std::unordered_map<L, Id, LanguageHash<L>> enodes;
    enodes.reserve(slots_.size());
    std::vector<Id> stack{egraph_.find(eclass)};

    while (!stack.empty()) {
      Id current = egraph_.find(stack.back());
      if (materialized.count(current)) {
        stack.pop_back();
        continue;
      }

      const auto &node = egraph_[current].nodes[bestEntry(current).node_index];
      bool ready = true;
      for (Id child : node.children()) {
        Id canonical_child = egraph_.find(child);
        if (!materialized.count(canonical_child)) {
          stack.push_back(canonical_child);
          ready = false;
          break;
        }
      }
      if (!ready) {
        continue;
      }

      L rebuilt = node.mapChildren(
          [&](Id child) { return materialized.at(egraph_.find(child)); });
      auto found = enodes.find(rebuilt);
      Id output;
      if (found != enodes.end()) {
        output = found->second;
      } else {
        output = expr.add(rebuilt);
        enodes.emplace(std::move(rebuilt), output);
      }
      materialized.emplace(current, output);
      stack.pop_back();
    }
    return expr;
  }

  const EGraph<L, A> &egraph_;
  mutable CostFn cost_fn_;
  std::unordered_map<Id, size_t> slots_;
  std::vector<std::optional<Entry>> best_;
  mutable std::vector<size_t> reach_marks_;
  mutable size_t reach_generation_ = 0;
};

} // namespace lotus::egraph
