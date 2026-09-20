#pragma once

#include "CFL/Classical/Core/Grammar.h"
#include "CFL/Classical/Core/Relation.h"

#include <cstddef>
#include <cstdint>
#include <memory>

namespace lotus::cfl::classical::engines {

/// Exact fixed-point statistics reported by the endpoint-quotient engine.
/// `logical_facts` counts concrete facts (seed plus inferred) without
/// expanding compressed rectangles; `cells` counts the occupied quotient
/// cells. The timing fields split preprocessing (partitions, lifts, bridges),
/// saturation, and exact counting, matching the underlying GEQ solver.
struct EndpointQuotientStatistics {
  std::size_t cells = 0;
  std::size_t logical_facts = 0;
  std::size_t seed_facts = 0;
  std::size_t inferred_facts = 0;
  std::size_t binary_joins = 0;
  std::size_t bridge_pairs = 0;
  std::size_t source_classes = 0;
  std::size_t target_classes = 0;
  std::size_t nullable_symbols = 0;
  std::size_t worklist_pops = 0;
  std::size_t peak_worklist = 0;
  std::size_t derived_facts =
      0; // Concrete facts added since the previous snapshot.
  std::size_t duplicate_facts = 0; // Rejected quotient-cell insertion attempts.
  std::size_t insert_attempts = 0;
  std::size_t binary_propagations = 0;
  std::size_t successful_binary_propagations = 0;
  std::size_t repeated_binary_outputs = 0;
  std::size_t binary_join_words = 0;
  std::size_t partitions_built = 0;
  std::size_t bridges_built = 0;
  std::size_t lifts_built = 0;
  std::uint64_t preprocess_us = 0;
  std::uint64_t saturation_us = 0;
  std::uint64_t count_us = 0;
};

/// Grammar-indexed endpoint-quotient (GEQ) engine adapter.
///
/// The engine buffers terminal input edges and, on `solve()`, builds a dense
/// `endpoint::Problem` from the normalized grammar and the buffered edges,
/// computes the exact least fixed point with the endpoint-quotient solver, and
/// retains the compressed result as a queryable Relation. Nullable diagonals
/// stay symbolic. Input edges are deduplicated so repeated
/// terminal insertions return false, matching the other session backends.
///
/// Adding terminal edges builds refined endpoint partitions, migrates the
/// previous compressed closure as already-processed cells, and saturates only
/// the delta. Queries see the last completed snapshot; buffered updates become
/// visible on the next solve. Traversal callbacks must not update or solve the
/// engine.
class EndpointQuotientEngine final : public Relation {
public:
  EndpointQuotientEngine(const Grammar &grammar, std::size_t node_count,
                         bool factorized = true);
  ~EndpointQuotientEngine() override;
  EndpointQuotientEngine(const EndpointQuotientEngine &) = delete;
  EndpointQuotientEngine &operator=(const EndpointQuotientEngine &) = delete;

  void ensureNodeCount(std::size_t node_count) override;
  /// Records a terminal input edge. Returns false when the edge was already
  /// buffered for the current session.
  bool add(SymbolId symbol, NodeId source, NodeId target) override;
  /// Rebuilds after updates; otherwise returns zero work counters and the
  /// existing snapshot's sizes. A failed solve preserves the previous result.
  EndpointQuotientStatistics solve();
  const EndpointQuotientStatistics &statistics() const;

  bool contains(SymbolId symbol, NodeId source, NodeId target) const override;
  bool visitSuccessors(SymbolId symbol, NodeId source,
                       NodeVisitor visitor) const override;
  bool visitPredecessors(SymbolId symbol, NodeId target,
                         NodeVisitor visitor) const override;
  bool visitEdges(EdgeVisitor visitor) const override;
  bool visitEdges(SymbolId symbol, EdgeVisitor visitor) const override;
  std::size_t edgeCount() const override;
  std::size_t edgeCount(SymbolId symbol) const override;
  std::size_t estimatedPayloadBytes() const override;
  std::size_t countOffDiagonalUnion(std::vector<SymbolId> symbols) const;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace lotus::cfl::classical::engines
