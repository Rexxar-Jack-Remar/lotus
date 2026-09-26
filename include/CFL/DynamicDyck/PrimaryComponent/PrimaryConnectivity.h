// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace lotus::cfl::dynamic_dyck::primary_component {

/// The PrimCompDS black box of Krishna et al., POPL 2024.
///
/// HDT level forests, represented by augmented Euler-tour treaps. Parallel
/// supports are counted; erasing one support does not erase another chain's
/// witness. Component handles provide O(1) representative queries; relabeling
/// visits only the smaller component on a genuine merge/split.
///
/// Treap balancing is randomized (fixed per-instance seed for reproducibility),
/// not a deterministic worst-case connectivity bound.
class PrimaryConnectivity {
public:
  using Node = std::size_t;
  struct Support {
    Node first, second;
    std::uint64_t count;
  };
  struct Statistics {
    std::size_t vertices = 0, edges = 0, components = 0, levels = 0;
    std::uint64_t insertions = 0, deletions = 0;
    std::uint64_t tree_links = 0, tree_cuts = 0;
    std::uint64_t replacements = 0, promotions = 0, scanned_non_tree = 0;
    std::uint64_t relabeled_vertices = 0;
  };

  PrimaryConnectivity();
  ~PrimaryConnectivity();
  PrimaryConnectivity(PrimaryConnectivity &&) noexcept;
  PrimaryConnectivity &operator=(PrimaryConnectivity &&) noexcept;
  PrimaryConnectivity(const PrimaryConnectivity &) = delete;
  PrimaryConnectivity &operator=(const PrimaryConnectivity &) = delete;

  Node addVertex();
  void insertEdge(Node first, Node second);
  /// Returns false for an absent support. Nodes must already exist.
  bool deleteEdge(Node first, Node second);
  Node representative(Node node) const;
  bool connected(Node first, Node second) const;
  Statistics statistics() const;

  /// Expensive diagnostic traversals, never used on production update paths.
  std::vector<Support> supports() const;
  bool validate(std::string *error = nullptr) const;

private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
};
} // namespace lotus::cfl::dynamic_dyck::primary_component
