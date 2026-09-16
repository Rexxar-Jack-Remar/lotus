#pragma once

#include "CFL/DynamicDyck/Statistics.h"

#include <string>
#include <vector>

namespace lotus::cfl::dynamic_dyck {

struct RunResult {
  // Original timing scopes: dynamic update calls, or recompute saturation
  // only; parsing, initial preprocessing, and graph copying are excluded.
  double elapsed_seconds = 0;
  std::size_t updates = 0;
  Statistics statistics;
  std::vector<std::vector<std::string>> components;
};

/// Original file workflow: preallocate endpoints and intern opaque node IDs
/// and parenthesis suffixes. Recompute after every record, including no-ops.
RunResult runFiles(bool dynamic, const std::string &initial_graph,
                   const std::string &update_sequence);

} // namespace lotus::cfl::dynamic_dyck
