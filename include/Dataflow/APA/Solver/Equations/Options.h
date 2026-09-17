#pragma once

#include "Dataflow/APA/Core/Options.h"

namespace elimination {

enum class PathSummaryEquationDirection {
  DependencyPrefix,
  ForwardPath,
};

struct PathSummaryEquationOptions final {
  PathSummaryEquationDirection Direction =
      PathSummaryEquationDirection::DependencyPrefix;
  // EAN/Greedy post-optimization of the solved summary batch, applied by
  // ForwardInterSummarySolver between summary solving and interpretation.
  // Default is a no-op pass, so the interprocedural baseline is unchanged.
  InterEANOptions EAN = {};
  OrderingPolicy Ordering = OrderingPolicy::Default;
  OrderPolicyOptions Order;
};

struct PathSummaryEquationDiagnostics final {
  std::size_t node_count = 0;
  std::size_t edge_count = 0;
  std::size_t scc_count = 0;
  std::size_t cyclic_scc_count = 0;
  // Total allocation is the host graph factory's lifetime count, including
  // input construction. These deltas distinguish warm repeated solves.
  std::size_t new_allocated_nodes = 0;
  std::size_t new_allocated_stars = 0;
  OrderingDiagnostics ordering;
};

} // namespace elimination
