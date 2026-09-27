#include "IR/UseHistory/ReachabilityIndex.h"
#include <algorithm>
#include <stdexcept>

namespace lotus {
namespace usehistory {
ReachabilityIndex::ReachabilityIndex(const FlowGraph &g) : G(g), Revision(g.revision()) {
  std::vector<std::vector<ID>> out(g.nodes().size()), in(g.nodes().size());
  for (const auto &e : g.edges()) if (e.enabled && !e.objects.empty()) {
    out[e.from].push_back(e.to); in[e.to].push_back(e.from);
  }
  std::vector<bool> visited(g.nodes().size());
  std::vector<ID> order;
  for (ID root = 0; root < g.nodes().size(); ++root) {
    if (visited[root]) continue;
    std::vector<std::pair<ID, std::size_t>> stack = {{root, 0}};
    visited[root] = true;
    while (!stack.empty()) {
      ID n = stack.back().first;
      if (stack.back().second < out[n].size()) {
        ID next = out[n][stack.back().second++];
        if (!visited[next]) { visited[next] = true; stack.push_back({next, 0}); }
      } else { order.push_back(n); stack.pop_back(); }
    }
  }
  Component.assign(g.nodes().size(), InvalidID);
  for (auto it = order.rbegin(); it != order.rend(); ++it) {
    if (Component[*it] != InvalidID) continue;
    ID component = static_cast<ID>(DAG.size()); DAG.emplace_back();
    std::vector<ID> stack = {*it}; Component[*it] = component;
    while (!stack.empty()) {
      ID n = stack.back(); stack.pop_back();
      for (auto pred : in[n]) if (Component[pred] == InvalidID) {
        Component[pred] = component; stack.push_back(pred);
      }
    }
  }
  for (ID n = 0; n < out.size(); ++n) for (auto next : out[n])
    if (Component[n] != Component[next]) DAG[Component[n]].push_back(Component[next]);
  for (auto &adj : DAG) {
    std::sort(adj.begin(), adj.end()); adj.erase(std::unique(adj.begin(), adj.end()), adj.end());
  }
}
void ReachabilityIndex::checkRevision() const {
  if (G.revision() != Revision) throw std::logic_error("UseHistory: stale reachability index; rebuild");
}
bool ReachabilityIndex::mayReach(FlowNodeID source, FlowNodeID sink) const {
  checkRevision();
  ID start = Component.at(source), target = Component.at(sink);
  auto found = Cache.find(start);
  if (found == Cache.end()) {
    std::vector<bool> seen(DAG.size()); std::vector<ID> stack = {start}; seen[start] = true;
    while (!stack.empty()) {
      ID n = stack.back(); stack.pop_back();
      for (auto next : DAG[n]) if (!seen[next]) { seen[next] = true; stack.push_back(next); }
    }
    std::vector<ID> closure;
    for (ID i = 0; i < seen.size(); ++i) if (seen[i]) closure.push_back(i);
    found = Cache.emplace(start, std::move(closure)).first;
  }
  return std::binary_search(found->second.begin(), found->second.end(), target);
}
bool ReachabilityIndex::mayReachAny(const std::vector<FlowNodeID> &sources,
                                    const std::vector<FlowNodeID> &sinks) const {
  checkRevision();
  for (auto source : sources) for (auto sink : sinks) if (mayReach(source, sink)) return true;
  return false;
}
} // namespace usehistory
} // namespace lotus
