// SPDX-License-Identifier: MIT
#pragma once

#include "CFL/DynamicDyck/Graph.h"
#include "CFL/DynamicDyck/Statistics.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace lotus::cfl::dynamic_dyck {

/// ReferenceCounted is Algorithm 2/3's Count semantics. Set matches
/// WeightedQuotientSolver.
enum class PrimaryComponentEdgeSemantics { ReferenceCounted, Set };

/// Both implement the paper's PrimCompDS interface. Deterministic is the
/// default O(n)-worst-case strong-certificate backend; HDT is the practical
/// alternative.
enum class PrimaryComponentConnectivityBackend { Deterministic, HDT };

namespace primary_component {
struct TestAccess;
} // namespace primary_component

struct PrimaryComponentCountedEdge {
  Edge edge; // Canonical opening orientation, as in the Lotus public API.
  std::uint64_t count;
};

struct PrimaryComponentDiagnostics {
  std::uint64_t edge_references = 0;
  std::uint64_t fixpoint_items = 0, fixpoint_targets = 0;
  std::uint64_t make_primary_calls = 0;
  std::uint64_t affected_components = 0, affected_vertices = 0;
  std::uint64_t sampled_out_targets = 0, in_primary_visits = 0;
  std::uint64_t rebuilt_summary_entries = 0;
  std::uint64_t primal_insertions = 0, primal_deletions = 0;
  std::uint64_t primal_replacements = 0, primal_promotions = 0;
  std::uint64_t primal_scanned_non_tree = 0, primal_relabeled_vertices = 0;
  std::size_t primal_edges = 0, primary_components = 0, primal_levels = 0;
  std::size_t certificate_universe = 0, certificate_blocks = 0;
  std::uint64_t certificate_rebuilds = 0, certificate_edges_scanned = 0;
  std::uint64_t certificate_vertices = 0, primal_representative_writes = 0;
  std::uint64_t primal_support_leaves_visited = 0,
                primal_path_nodes_visited = 0;
  std::uint64_t dscc_parent_steps = 0, dscc_cache_vertices = 0;
};

/// DynamicAlgo of Krishna, Lal, Pavlogiannis and Tuppe, POPL 2024.
///
/// Exact reachability for ONE Dyck language on BIDIRECTED graphs. Every Edge
/// denotes an edge together with its complementary reverse, not an independent
/// directed arc. Public edges use Lotus's types; internally edges are closing.
/// No epsilon-edge encoding is implicit. Labels are arbitrary unsigned values.
///
/// Insert/delete synchronously complete Algorithms 2/3, MakePrimary (Algorithm
/// 4) and Fixpoint (Algorithm 1). No full-input Dyck-reachability recomputation
/// is used. The default Deterministic backend meets the required O(n)
/// PrimCompDS bound. With constant alphabet size, updates take O(n alpha(n))
/// worst-case work in the RAM model. HDT remains available, but does not carry
/// this guarantee.
///
/// Arbitrary Lotus vertex-ID lookup uses an ordered map: O(log n). Dense vertex
/// indices give O(1) representative/connectivity queries in Deterministic mode.
/// Access to one instance requires external synchronization; HDT-mode queries
/// may compress union-find paths even through a const reference.
class PrimaryComponentSolver {
public:
  using VertexIndex = std::size_t;

  explicit PrimaryComponentSolver(
      PrimaryComponentEdgeSemantics semantics =
          PrimaryComponentEdgeSemantics::ReferenceCounted,
      PrimaryComponentConnectivityBackend backend =
          PrimaryComponentConnectivityBackend::Deterministic);
  explicit PrimaryComponentSolver(
      const Graph &graph,
      PrimaryComponentEdgeSemantics semantics =
          PrimaryComponentEdgeSemantics::ReferenceCounted,
      PrimaryComponentConnectivityBackend backend =
          PrimaryComponentConnectivityBackend::Deterministic);
  ~PrimaryComponentSolver();
  PrimaryComponentSolver(PrimaryComponentSolver &&) noexcept;
  PrimaryComponentSolver &operator=(PrimaryComponentSolver &&) noexcept;
  PrimaryComponentSolver(const PrimaryComponentSolver &) = delete;
  PrimaryComponentSolver &operator=(const PrimaryComponentSolver &) = delete;

  bool addVertex(Vertex vertex);
  /// Adds missing endpoints. Returns whether the reference count changed.
  /// In counted mode an explicit complementary reverse adds another reference.
  bool insertEdge(Edge edge);
  /// Removes ONE reference (or the pair in Set mode). Absent pairs are no-ops
  /// and never add vertices. Returns whether the reference count changed.
  bool deleteEdge(Edge edge);
  bool apply(const Update &update);

  bool connected(Vertex source, Vertex target) const;
  /// Indices are stable for the lifetime of a graph state, including moves, but
  /// belong to one solver: do not transfer indices between unrelated instances.
  /// Resolving a Vertex to its index costs O(log n); subsequent index queries
  /// cost O(1) in Deterministic mode. Invalid indices throw out_of_range.
  VertexIndex vertexIndex(Vertex vertex) const;
  Vertex vertexAt(VertexIndex index) const;
  bool connectedByIndex(VertexIndex source, VertexIndex target) const;
  Vertex representativeByIndex(VertexIndex vertex) const;
  Vertex primaryRepresentativeByIndex(VertexIndex vertex) const;
  /// Returns an original vertex ID. Unknown vertices throw out_of_range.
  /// Representatives may change after either kind of update.
  Vertex representative(Vertex vertex) const;
  Vertex primaryRepresentative(Vertex vertex) const;
  std::vector<std::vector<Vertex>> components() const;
  /// Unique pairs only, in canonical opening orientation. Use edgeCounts() for
  /// a lossless view of multiplicities. Both exports are deterministically
  /// sorted.
  Graph graph() const;
  std::vector<PrimaryComponentCountedEdge> edgeCounts() const;
  std::uint64_t edgeMultiplicity(Edge edge) const;
  Statistics statistics() const;
  PrimaryComponentDiagnostics diagnostics() const;
  PrimaryComponentEdgeSemantics edgeSemantics() const;
  PrimaryComponentConnectivityBackend connectivityBackend() const;

  /// Expensive structural invariant check, for tests/debugging only. Does not
  /// replace the independent semantic oracle in the accompanying tests.
  bool validate(std::string *error = nullptr) const;

private:
  friend struct primary_component::TestAccess;
  class Impl;
  std::unique_ptr<Impl> m_impl;
  Impl &impl() const;
};
} // namespace lotus::cfl::dynamic_dyck
