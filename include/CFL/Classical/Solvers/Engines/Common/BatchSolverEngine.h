#pragma once

#include "CFL/Classical/Core/Grammar.h"
#include "CFL/Classical/Core/Relation.h"

#include <cstddef>
#include <memory>

namespace lotus::cfl::classical::engines {

enum class BatchEngineKind { Cat, Iea, IeaOcr };

struct BatchSolverStatistics {
  std::size_t nodes = 0;
  std::size_t unique_base_edges = 0;
  std::size_t expanded_facts = 0;
  std::size_t stored_facts = 0;
  std::size_t derived_facts = 0;
  std::size_t attempts = 0;
  std::size_t successful_insertions = 0;
  std::size_t duplicate_attempts = 0;
  std::size_t work_items = 0;
  std::size_t peak_worklist = 0;
  std::size_t unary_applications = 0;
  std::size_t binary_join_pairs = 0;

  std::size_t cat_graph_degree = 0;
  std::size_t cat_fully_pruned_attempts = 0;
  std::size_t cat_incoming_only_insertions = 0;
  std::size_t cat_outgoing_only_insertions = 0;
  std::size_t cat_fully_indexed_insertions = 0;
  std::size_t cat_unindexed_insertions = 0;
  std::size_t cat_propagating_insertions = 0;
  std::size_t cat_dynamic_insertions = 0;
  std::size_t cat_promotions = 0;
  std::size_t cat_context_annotations = 0;
  std::size_t cat_universal_contexts = 0;
  std::size_t cat_rewrites = 0;

  std::size_t ieoce_quotient_nodes = 0;
  std::size_t ieoce_epochs = 0;
  std::size_t ieoce_scc_passes = 0;
  std::size_t ieoce_collapsed_components = 0;
  std::size_t ieoce_merged_nodes = 0;
  std::size_t ieoce_quotient_replays = 0;
  std::size_t ieoce_graph_facts = 0;
  std::size_t ieoce_meg_insertions = 0;
  std::size_t ieoce_meg_edges = 0;
  std::size_t ieoce_meg_edges_removed = 0;
  std::size_t ieoce_transitive_updates = 0;
  std::size_t ieoce_ordered_steps = 0;
  std::size_t ieoce_ordered_prunes = 0;
  bool ieoce_ordinary_fallback = false;
  bool ieoce_ordered_enabled = false;
};

/// Native Relation adapter for CAT, IEA, and IEA-OCR.
///
/// Changed input batches rebuild because CAT contexts and IEOCE quotient
/// partitions are static snapshots. IEOCE answers remain compressed until a
/// Relation traversal explicitly asks to enumerate their concrete facts.
/// SolverSession filters unchanged calls before dispatch.
class BatchSolverEngine final : public Relation {
public:
  BatchSolverEngine(const Grammar &grammar, std::size_t node_count,
                    BatchEngineKind variant);
  ~BatchSolverEngine() override;
  BatchSolverEngine(const BatchSolverEngine &) = delete;
  BatchSolverEngine &operator=(const BatchSolverEngine &) = delete;

  void ensureNodeCount(std::size_t node_count) override;
  bool add(SymbolId symbol, NodeId source, NodeId target) override;
  BatchSolverStatistics solve();
  const BatchSolverStatistics &statistics() const;

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

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace lotus::cfl::classical::engines
