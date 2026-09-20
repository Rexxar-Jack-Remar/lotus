#pragma once

#include "CFL/Classical/Core/Grammar.h"
#include "CFL/Classical/Core/Graph.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace lotus::cfl::classical {

struct EndpointQuotientRuleProfile {
  std::size_t rule_id = 0;
  std::size_t kind = 0;
  std::size_t lhs = 0;
  std::size_t left = 0;
  std::size_t right = 0;
  std::size_t delta_rows = 0;
  std::size_t delta_cells = 0;
  std::size_t joins = 0;
  std::size_t propagations = 0;
  std::size_t successful_propagations = 0;
  std::size_t repeated_outputs = 0;
  std::size_t join_word_operations = 0;
};

struct EndpointQuotientSccProfile {
  std::size_t scc_id = 0;
  /// Numeric value of engines::EndpointQuotientSccClass. Kept numeric here to
  /// avoid coupling the general session interface to a concrete engine.
  std::size_t classification = 0;
  std::size_t symbols = 0;
  std::size_t rules = 0;
  std::size_t delta_rows = 0;
  std::size_t delta_cells = 0;
  std::size_t joins = 0;
  std::size_t propagations = 0;
  std::size_t successful_propagations = 0;
  std::size_t repeated_outputs = 0;
  std::size_t join_word_operations = 0;
};

struct ReachabilityStats {
  // Session snapshots after this solve.
  std::size_t graph_nodes = 0;
  std::size_t base_graph_edges = 0;
  std::size_t grammar_symbols = 0;
  std::size_t grammar_terminals = 0;
  std::size_t grammar_nonterminals = 0;
  std::size_t grammar_productions = 0;
  std::size_t grammar_nullable_symbols = 0;
  std::size_t grammar_transitive_symbols = 0;
  std::size_t input_edges = 0;
  std::size_t relation_edges = 0;
  std::size_t start_symbol_edges = 0;
  std::size_t count_symbol_edges = 0;
  std::size_t relation_payload_bytes_estimate = 0;
  std::size_t transitive_closure_instances = 0;
  std::size_t transitive_relation_edges = 0;
  std::size_t transitive_payload_bytes_estimate = 0;
  std::size_t pocr_tree_roots = 0;
  std::size_t pocr_tree_nodes = 0;
  std::size_t pocr_tree_edges = 0;
  std::size_t fully_ordered_critical_edges = 0;
  std::size_t candidate_relation_edges = 0;

  // Work performed by this solve call only.
  std::uint64_t classical_iterations = 0;
  std::size_t processed_work_items = 0;
  std::size_t duplicate_edges = 0;
  std::size_t peak_worklist_size = 0;
  std::size_t added_edges = 0;
  std::uint64_t solve_time_microseconds = 0;
  /// Optional client-side timings; zero for graph-only solver sessions.
  std::uint64_t frontend_time_microseconds = 0;
  std::uint64_t client_initialization_microseconds = 0;
  std::uint64_t client_discovery_microseconds = 0;
  std::size_t transitive_arc_insertions = 0;
  std::size_t transitive_propagated_pairs = 0;
  std::size_t transitive_duplicate_pairs = 0;
  std::size_t pocr_traversal_steps = 0;
  std::size_t pocr_tree_join_visits = 0;
  std::size_t fully_ordered_reachability_checks = 0;
  std::size_t fully_ordered_tree_join_visits = 0;
  std::size_t fully_ordered_critical_edge_insertions = 0;
  std::size_t fully_ordered_critical_edge_removals = 0;
  std::size_t fully_ordered_cycle_simplifications = 0;
  std::size_t graspan_epochs = 0;
  std::size_t skewed_indexed_facts = 0;
  std::size_t skewed_propagating_facts = 0;
  std::size_t skewed_propagating_symbols = 0;
  std::size_t skewed_dynamic_eligible_symbols = 0;
  std::size_t skewed_static_pe_insertions = 0;
  std::size_t skewed_dynamic_pe_insertions = 0;
  std::size_t skewed_promotions_to_indexed = 0;
  std::size_t skewed_unary_applications = 0;
  std::size_t skewed_binary_join_pairs = 0;
  std::size_t batch_stored_facts = 0;
  std::size_t cat_graph_degree = 0;
  std::size_t cat_fully_pruned_attempts = 0;
  std::size_t cat_context_annotations = 0;
  std::size_t cat_rewrites = 0;
  std::size_t ieoce_quotient_nodes = 0;
  std::size_t ieoce_epochs = 0;
  std::size_t ieoce_merged_nodes = 0;
  std::size_t ieoce_graph_facts = 0;
  std::size_t ieoce_meg_edges = 0;
  std::size_t ieoce_meg_edges_removed = 0;
  std::size_t ieoce_ordered_steps = 0;
  bool ieoce_ordinary_fallback = false;
  std::size_t endpoint_quotient_cells = 0;
  std::size_t endpoint_quotient_facts = 0;
  std::size_t endpoint_quotient_seed_facts = 0;
  std::size_t endpoint_quotient_inferred_facts = 0;
  std::size_t endpoint_quotient_binary_joins = 0;
  std::size_t endpoint_quotient_bridge_pairs = 0;
  std::size_t endpoint_quotient_source_classes = 0;
  std::size_t endpoint_quotient_target_classes = 0;
  std::size_t endpoint_quotient_nullable_symbols = 0;
  std::uint64_t endpoint_quotient_preprocess_us = 0;
  std::uint64_t endpoint_quotient_saturation_us = 0;
  std::uint64_t endpoint_quotient_count_us = 0;
  std::size_t endpoint_quotient_insert_attempts = 0;
  std::size_t endpoint_quotient_duplicate_inserts = 0;
  std::size_t endpoint_quotient_binary_propagations = 0;
  std::size_t endpoint_quotient_successful_binary_propagations = 0;
  std::size_t endpoint_quotient_repeated_binary_outputs = 0;
  std::size_t endpoint_quotient_binary_join_words = 0;
  std::size_t endpoint_quotient_partitions_built = 0;
  std::size_t endpoint_quotient_bridges_built = 0;
  std::size_t endpoint_quotient_lifts_built = 0;
  std::size_t endpoint_quotient_dependency_sccs = 0;
  std::size_t endpoint_quotient_acyclic_sccs = 0;
  std::size_t endpoint_quotient_unary_recursive_sccs = 0;
  std::size_t endpoint_quotient_transitive_sccs = 0;
  std::size_t endpoint_quotient_linear_sccs = 0;
  std::size_t endpoint_quotient_general_sccs = 0;
  std::size_t endpoint_quotient_max_scc_symbols = 0;
  std::size_t endpoint_quotient_max_scc_rules = 0;
  std::size_t endpoint_quotient_hottest_rule_id = 0;
  std::size_t endpoint_quotient_hottest_rule_joins = 0;
  std::size_t endpoint_quotient_hottest_scc_id = 0;
  std::size_t endpoint_quotient_hottest_scc_joins = 0;
  std::vector<EndpointQuotientRuleProfile> endpoint_quotient_per_rule;
  std::vector<EndpointQuotientSccProfile> endpoint_quotient_per_scc;

  // Aggregates report how many solve calls they combine.
  std::size_t solver_rounds = 1;
};

enum class SolverBackend {
  /// Classical worklist with hash-set relations.
  SparseSet,
  /// Same classical worklist with LLVM sparse-bitvector relations.
  SparseBitVector,
  /// Graspan-style epoch/delta evaluation with sparse-bitvector relations.
  Graspan,
  /// Sqid adaptive and differential relation chaining.
  Sqid,
  /// PEARL transitivity-aware multi-derivation.
  Pearl,
  /// PLDI 2024 skewed tabulation with separate indexed and propagating facts.
  Skewed,
  /// ICSE 2026 context-aware tabulation.
  Cat,
  /// OOPSLA 2024 iterative-epoch online cycle elimination.
  Iea,
  /// IEA with online cycle reduction and minimum-equivalent graphs.
  IeaOcr,
  /// Classical worklist plus dedicated incremental closure only for symbols
  /// having a literal production A -> A A.
  TransitiveClosure,
  /// POCR paired predecessor/successor reachability-tree propagation.
  Pocr,
  /// POCR with transitive facts prioritized ahead of ordinary work items.
  HierarchicalPocr,
  /// Fully ordered CFL reachability with an edge-critical graph.
  FullyOrdered,
  /// Grammar-indexed endpoint-quotient (GEQ) compressed exact solving.
  EndpointQuotient,
};

struct SolverOptions {
  SolverBackend backend = SolverBackend::SparseSet;
  /// Honor POCR Insert/Follow metadata by indexing only terminals, nullable
  /// seeds, and Insert symbols as future join candidates.
  bool unidirectional = false;
  /// Apply POCR's optional ECG SCC simplification in the FOCR backend.
  bool simplify_focr_cycles = false;
  /// Explicit X/Xbar pairs for PEARL's PackRR and paired propagation graphs.
  std::vector<std::pair<std::string, std::string>> pearl_inverse_relations;
  /// Preserve production-local endpoint factors in EndpointQuotient.
  bool endpoint_quotient_factorized = false;
};

const char *solverBackendName(SolverBackend backend);
SolverBackend parseSolverBackend(std::string_view name);

/// Retains the derived relation and supports adding terminal edges followed by
/// another run to a fixed point.
class SolverSession {
public:
  SolverSession(LabeledGraph &graph, const Grammar &grammar,
                SolverBackend backend = SolverBackend::SparseSet);
  SolverSession(LabeledGraph &graph, const Grammar &grammar,
                const SolverOptions &options);
  ~SolverSession();
  SolverSession(SolverSession &&) noexcept;
  SolverSession &operator=(SolverSession &&) noexcept;
  SolverSession(const SolverSession &) = delete;
  SolverSession &operator=(const SolverSession &) = delete;

  std::size_t addNode(const std::string &name);
  bool addTerminalEdge(std::size_t source, std::size_t target,
                       const std::string &label);
  ReachabilityStats solve();
  bool contains(std::size_t source, std::size_t target,
                const std::string &label) const;
  const Relation &relation() const;

private:
  friend class AliasClient;

  /// Seed a previously derived fact while migrating to a monotonic grammar
  /// extension. This does not mutate the input graph.
  bool addKnownRelationEdge(std::size_t source, std::size_t target,
                            const std::string &label);

  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace lotus::cfl::classical
