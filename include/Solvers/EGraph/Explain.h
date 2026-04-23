#pragma once

#include "Solvers/EGraph/EGraph.h"

#include <queue>

namespace lotus::egraph {

struct ExplanationStep {
  Id left;
  Id right;
  std::string reason;
};

class Explanation {
public:
  Explanation() = default;
  explicit Explanation(std::vector<ExplanationStep> steps)
      : steps_(std::move(steps)) {}

  const std::vector<ExplanationStep> &steps() const { return steps_; }

private:
  std::vector<ExplanationStep> steps_;
};

template <typename L, typename A>
inline std::optional<Explanation> explainEquivalence(const EGraph<L, A> &egraph,
                                                     Id lhs, Id rhs) {
  if (egraph.find(lhs) != egraph.find(rhs)) {
    return std::nullopt;
  }

  if (lhs == rhs) {
    return Explanation(std::vector<ExplanationStep>{});
  }

  struct Edge {
    Id next;
    size_t event_index = 0;
  };

  std::unordered_map<Id, std::vector<Edge>> graph;
  const auto &events = egraph.unionEvents();
  for (size_t i = 0; i < events.size(); ++i) {
    graph[events[i].left].push_back({events[i].right, i});
    graph[events[i].right].push_back({events[i].left, i});
  }

  std::queue<Id> worklist;
  std::unordered_set<Id> visited;
  std::unordered_map<Id, std::pair<Id, size_t>> previous;

  visited.insert(lhs);
  worklist.push(lhs);

  bool found = false;
  while (!worklist.empty() && !found) {
    Id current = worklist.front();
    worklist.pop();

    for (const auto &edge : graph[current]) {
      if (!visited.insert(edge.next).second) {
        continue;
      }
      previous[edge.next] = {current, edge.event_index};
      if (edge.next == rhs) {
        found = true;
        break;
      }
      worklist.push(edge.next);
    }
  }

  if (!found) {
    return std::nullopt;
  }

  std::vector<ExplanationStep> steps;
  for (Id current = rhs; current != lhs; current = previous[current].first) {
    const auto &event = events[previous[current].second];
    steps.push_back({event.left, event.right, event.reason});
  }
  std::reverse(steps.begin(), steps.end());
  return Explanation(std::move(steps));
}

} // namespace lotus::egraph
