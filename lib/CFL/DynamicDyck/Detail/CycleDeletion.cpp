#include "CFL/DynamicDyck/Detail/Engine.h"

#include <deque>
#include <unordered_map>
#include <unordered_set>

namespace lotus::cfl::dynamic_dyck::detail {

bool Engine::hasAffectedCycle(CFLHashMap &graph, unsigned source,
                              unsigned target) {
  std::unordered_set<unsigned> region;
  std::vector<unsigned> nodes;
  auto visit = [&](unsigned node) {
    if (region.insert(node).second)
      nodes.push_back(node);
  };
  visit(source);
  visit(target);
  for (std::size_t index = 0; index < nodes.size(); ++index) {
    std::unordered_map<unsigned, Matrix1> incoming;
    graph.CheckInEdges(nodes[index], incoming);
    for (const auto &edge : incoming)
      if (!edge.second.colors.empty())
        visit(edge.first);
  }
  std::unordered_map<unsigned, std::size_t> indegree;
  for (unsigned node : nodes)
    indegree.emplace(node, 0);
  for (unsigned node : nodes) {
    std::unordered_map<unsigned, Matrix1> outgoing;
    graph.CheckOutEdges(node, outgoing);
    for (const auto &edge : outgoing)
      if (!edge.second.colors.empty() && region.count(edge.first))
        ++indegree.at(edge.first);
  }
  std::deque<unsigned> ready;
  for (const auto &entry : indegree)
    if (entry.second == 0)
      ready.push_back(entry.first);
  std::size_t processed = 0;
  while (!ready.empty()) {
    const unsigned node = ready.front();
    ready.pop_front();
    ++processed;
    std::unordered_map<unsigned, Matrix1> outgoing;
    graph.CheckOutEdges(node, outgoing);
    for (const auto &edge : outgoing)
      if (!edge.second.colors.empty() && region.count(edge.first) &&
          --indegree.at(edge.first) == 0)
        ready.push_back(edge.first);
  }
  return processed != nodes.size();
}

void Engine::rebuild(
    CFLHashMap &original, CFLHashMap &merged,
    std::unordered_map<std::string, In_FastDLL<unsigned>> &colors,
    std::unordered_map<unsigned, std::list<unsigned>> &sets) {
  // Cyclic weights can retain false anchors after deletion. Re-running the
  // original preprocessing on the remaining graph computes its least closure.
  ++cycle_rebuild_count;
  split_count += original.GetVtxNum() - sets.size();
  merged.CopyFrom(original);
  arrayreach(merged, colors, sets);
}

} // namespace lotus::cfl::dynamic_dyck::detail
