#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace lotus {
namespace ufg {
using SearchID = std::uint64_t;
namespace detail {
inline std::uint64_t expandedNodeCount(std::size_t nodes, std::size_t objects) {
  if (objects && nodes > std::numeric_limits<std::uint64_t>::max() / objects)
    throw std::length_error("UFG: expanded node identifier space exhausted");
  return std::uint64_t(nodes) * objects;
}
} // namespace detail
} // namespace ufg
} // namespace lotus
