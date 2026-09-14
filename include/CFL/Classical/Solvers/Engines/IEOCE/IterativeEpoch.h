// SPDX-License-Identifier: MIT
// Xu et al., OOPSLA 2024, DOI 10.1145/3649862, Sections 4-5 / Algorithms 2-5.
#pragma once
#include "CFL/Classical/Solvers/Engines/Common/Reachability.h"

namespace lotus::cfl::classical::ieoce {
using common::Fact;
using common::Grammar;
using common::Graph;
using common::Limits;
using common::Node;
using common::Query;
using common::Reachability;
using common::Rule;
using common::Symbol;
enum class Direction { Directed, Bidirected };
enum class Variant { Iea, IeaOcr };
struct TransitivityCheck {
  Symbol symbol = 0;
  bool eligible = false;
  bool doubly_recursive = false;
  std::vector<Symbol> nonabsorbing_targets;
  std::vector<Rule> intransitive_combinations;
};
// Definitions 4.1-4.6, checked against ALL requested nonterminals, not merely
// S. Bidirected mode requires a total involution and a syntactically
// reverse-closed grammar; solve additionally verifies reverse closure of
// terminal input edges.
TransitivityCheck
checkTransitiveSymbol(const Grammar &grammar, Symbol symbol,
                      const Query &query = {},
                      Direction direction = Direction::Directed,
                      const std::vector<Symbol> &reverse_symbols = {});
std::vector<Symbol>
findTransitiveSymbols(const Grammar &grammar, const Query &query = {},
                      Direction direction = Direction::Directed,
                      const std::vector<Symbol> &reverse_symbols = {});

struct Options {
  Query query;
  Limits limits;
  Direction direction = Direction::Directed;
  Variant variant = Variant::Iea;
  std::vector<Symbol> reverse_symbols;
  // Empty => deterministically choose the first admissible symbol, preferring
  // an AA rule for IeaOcr. Explicitly requesting an unsafe symbol is an error.
  std::optional<Symbol> transitive_symbol;
  // Generic grammars may have no collapsible relation. Unless this flag is
  // set, solve then performs exact ordinary tabulation and reports the
  // fallback.
  bool require_optimization = false;
  bool trace_epochs = false;
  // Expensive diagnostic checks: quotient canonicalization and, for IeaOcr,
  // equality of MEG reachability/main A closure and post-collapse minimality.
  bool check_invariants = false;
};
struct EpochStats {
  std::size_t epoch = 0, generating_work = 0, other_work = 0, nodes_merged = 0;
  std::size_t remaining_nodes = 0, graph_facts = 0, meg_edges = 0;
};
struct Stats {
  std::size_t nodes = 0, unique_base_edges = 0, quotient_nodes = 0;
  std::size_t epochs = 0, scc_passes = 0, collapsed_components = 0,
              merged_nodes = 0;
  std::size_t attempts = 0, successful_insertions = 0, duplicate_attempts = 0;
  std::size_t work_items = 0, generating_work = 0, other_work = 0,
              peak_worklist = 0;
  std::size_t unary_applications = 0, binary_join_pairs = 0;
  std::size_t quotient_replays = 0, graph_facts = 0;
  std::size_t meg_insertions = 0, meg_edges = 0, meg_edges_removed = 0;
  std::size_t transitive_updates = 0, ordered_steps = 0, ordered_prunes = 0;
  bool ordinary_fallback = false, ordered_enabled = false;
};
struct Result {
  Reachability reachability;
  Stats stats;
  std::optional<Symbol> transitive_symbol;
  std::vector<Symbol> admissible_symbols;
  std::vector<EpochStats> epoch_trace;
  bool contains(Node u, Symbol s, Node v) const {
    return reachability.contains(u, s, v);
  }
};
Result solve(const Grammar &grammar, const Graph &graph,
             const Options &options = {});
using RebuildingSession = common::RebuildingSession<Options, Result>;
} // namespace lotus::cfl::classical::ieoce
