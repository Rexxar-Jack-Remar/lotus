#include "CFL/DynamicDyck/IO.h"

#include "CFL/DynamicDyck/Detail/Engine.h"

#include <algorithm>
#include <unordered_map>

namespace lotus::cfl::dynamic_dyck {

RunResult runFiles(bool dynamic, const std::string &initial_graph,
                   const std::string &update_sequence) {
  detail::Engine engine;
  engine.arrayversion(dynamic, initial_graph, update_sequence);
  RunResult result;
  result.elapsed_seconds = engine.elapsed;
  result.updates = engine.update_count;
  result.statistics.vertices = engine.node_names.size();
  result.statistics.edges = engine.final_edge_count;
  result.statistics.insertions = engine.insertion_count;
  result.statistics.deletions = engine.deletion_count;
  result.statistics.merges = engine.merge_count;
  result.statistics.splits = engine.split_count;
  result.statistics.cycle_rebuilds = engine.cycle_rebuild_count;
  std::unordered_map<unsigned, std::vector<std::string>> components;
  for (unsigned node = 0; node < engine.node_names.size(); ++node)
    components[engine.resp->find(node)].push_back(engine.node_names[node]);
  for (auto &component : components) {
    std::sort(component.second.begin(), component.second.end());
    result.components.push_back(std::move(component.second));
  }
  std::sort(result.components.begin(), result.components.end());
  result.statistics.components = result.components.size();
  return result;
}

} // namespace lotus::cfl::dynamic_dyck
