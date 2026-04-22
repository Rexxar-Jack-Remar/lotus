#pragma once

#include <cstddef>
#include <vector>

namespace vasco {

template <typename N> class DirectedGraph {
public:
  virtual ~DirectedGraph() = default;

  virtual std::vector<N> nodes() const = 0;
  virtual std::vector<N> heads() const = 0;
  virtual std::vector<N> tails() const = 0;
  virtual std::vector<N> predsOf(const N &Node) const = 0;
  virtual std::vector<N> succsOf(const N &Node) const = 0;
  virtual std::size_t size() const = 0;
};

} // namespace vasco
