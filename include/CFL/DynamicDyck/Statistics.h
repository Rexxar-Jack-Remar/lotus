#pragma once

#include <cstddef>

namespace lotus::cfl::dynamic_dyck {

struct Statistics {
  std::size_t vertices = 0;
  // Unique bidirected pairs, counting only canonical opening edges.
  std::size_t edges = 0;
  std::size_t components = 0;
  std::size_t insertions = 0;
  std::size_t deletions = 0;
  std::size_t merges = 0;
  std::size_t splits = 0;
  std::size_t cycle_rebuilds = 0;
};

} // namespace lotus::cfl::dynamic_dyck
