// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace lotus::cfl::dynamic_dyck::primary_component {

/// Deterministic implementation of DynamicAlgo's PrimCompDS black-box contract.
///
/// A vertex-pair partition tree stores a spanning-forest certificate at every
/// occupied block. A support update changes one leaf-to-root path only. Child
/// forests, not original adjacency lists, are scanned on that path. Their sizes
/// form a geometric series, giving O(n) worst-case work per support update and
/// O(1) component-representative queries. Parallel supports are reference
/// counted.
///
/// This is a simpler strong-certificate sparsification instantiation, NOT the
/// O(sqrt(n)) Eppstein et al. dynamic-connectivity implementation. O(n)
/// suffices for the enclosing paper's O(n alpha(n)) bound.
/// Allocation/deallocation is charged per word in the
/// standard RAM model; this is not a hard-real-time guarantee about the C++
/// allocator or operating system.
class SparsifiedConnectivity {
public:
  using Node = std::size_t;
  struct Support {
    Node first, second;
    std::uint64_t count;
  };
  struct Statistics {
    std::size_t vertices = 0, edges = 0, components = 0;
    std::size_t universe = 1, levels = 1, blocks = 0;
    std::uint64_t insertions = 0, deletions = 0;
    std::uint64_t certificate_rebuilds = 0, certificate_edges_scanned = 0;
    std::uint64_t certificate_vertices = 0, representative_writes = 0;
    std::uint64_t support_leaves_visited = 0, path_nodes_visited = 0;
  };

  SparsifiedConnectivity();
  ~SparsifiedConnectivity();
  SparsifiedConnectivity(SparsifiedConnectivity &&) noexcept;
  SparsifiedConnectivity &operator=(SparsifiedConnectivity &&) noexcept;
  SparsifiedConnectivity(const SparsifiedConnectivity &) = delete;
  SparsifiedConnectivity &operator=(const SparsifiedConnectivity &) = delete;

  Node addVertex();
  void insertEdge(Node first, Node second);
  bool deleteEdge(Node first, Node second);
  Node representative(Node node) const;
  bool connected(Node first, Node second) const;
  Statistics statistics() const;

  // Diagnostic traversals, outside the update/query bounds.
  std::vector<Support> supports() const;
  bool validate(std::string *error = nullptr) const;

private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
  Impl &impl() const;
};
} // namespace lotus::cfl::dynamic_dyck::primary_component
