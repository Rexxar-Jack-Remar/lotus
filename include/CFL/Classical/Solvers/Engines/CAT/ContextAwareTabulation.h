// SPDX-License-Identifier: MIT
// Shi and Li, ICSE 2026, DOI 10.1145/3744916.3773222.
// Independent implementation of Figure 3 and Algorithm 2. See PAPER_NOTES.md
// for explicitly resolved pseudocode omissions and guarded grammar rewrites.
#pragma once
#include "CFL/Classical/Solvers/Engines/Common/Reachability.h"

#include <unordered_set>

namespace lotus::cfl::classical::cat {
using common::Fact;
using common::Grammar;
using common::Graph;
using common::Limits;
using common::Node;
using common::Query;
using common::Reachability;
using common::ResourceLimit;
using common::Rule;
using common::Symbol;
struct TerminalSet {
  std::vector<Symbol> terminals; // Sorted, unique, no epsilon sentinel.
  bool epsilon = false;
  bool contains(Symbol terminal) const;
};
enum class FirstLastSemantics {
  // Literal Figure-3 inference, including its conservative epsilon propagation.
  PaperInferenceRules,
  // Definitions 4.1/4.2: a binary RHS is nullable iff BOTH operands are
  // nullable.
  Definitions
};
struct GrammarAnalysis {
  std::vector<TerminalSet> first, last, left, right;
  std::vector<bool> unary_uses;
};
GrammarAnalysis analyzeGrammar(
    const Grammar &grammar,
    FirstLastSemantics semantics = FirstLastSemantics::PaperInferenceRules);

struct UsageContexts {
  std::vector<Node> nodes;
  std::vector<bool> incoming_everywhere, outgoing_everywhere;
  // Sparse annotations. Universally true contexts occupy only the flags above.
  std::vector<std::unordered_set<Node>> incoming, outgoing;
  bool hasIncoming(Node node, Symbol symbol) const;
  bool hasOutgoing(Node node, Symbol symbol) const;
};
UsageContexts annotateGraph(const Grammar &grammar, const Graph &graph,
                            const GrammarAnalysis &analysis);

enum class RewriteKind { EpsilonBasedClosureOrientation, TransitiveCore };
struct RewriteEvent {
  RewriteKind kind;
  Symbol symbol;
  std::optional<Symbol> core;
};
struct Transformation {
  Grammar grammar;
  std::vector<RewriteEvent> events;
  std::vector<Symbol> nonnullable_transitives_skipped;
  std::vector<Symbol> unsafe_orientation_changes_skipped;
};
// Section 4.2.1, language-preserving applicability guards:
// * do not add epsilon to a nonnullable transitive symbol;
// * reverse prefix-closure orientation only for an epsilon-based, one-sided
//   closure. General B X -> X B replacement is not language preserving.
// * expand left and right consumers in their ORIGINAL order otherwise.
Transformation rewriteTransitivity(const Grammar &grammar);

struct Options {
  Query query;
  Limits limits;
  FirstLastSemantics first_last = FirstLastSemantics::PaperInferenceRules;
  bool rewrite_transitivity = true;
  // Optional reuse of the earlier, explicitly limited static Skewed pass.
  bool conservative_static_skewing = false;
  bool propagating_symbols = true;
  bool dynamic_skewing = true;
};
struct Stats {
  std::size_t nodes = 0, unique_base_edges = 0;
  std::size_t attempts = 0, duplicate_attempts = 0, fully_pruned_attempts = 0;
  std::size_t successful_insertions = 0, work_items = 0, peak_worklist = 0;
  std::size_t unary_applications = 0, binary_join_pairs = 0;
  std::size_t graph_degree = 0, incoming_only_insertions = 0,
              outgoing_only_insertions = 0;
  std::size_t fully_indexed_insertions = 0, unindexed_insertions = 0;
  std::size_t propagating_insertions = 0, dynamic_insertions = 0,
              promotions = 0;
  std::size_t context_annotations = 0, universal_contexts = 0;
};
struct Result {
  Reachability reachability;
  Stats stats;
  Grammar working_grammar;
  GrammarAnalysis analysis;
  Transformation transformation;
  std::vector<skewed::Rewrite> static_skewing_rewrites;
  bool contains(Node u, Symbol s, Node v) const {
    return reachability.contains(u, s, v);
  }
};
Result solve(const Grammar &grammar, const Graph &graph,
             const Options &options = {});
using RebuildingSession = common::RebuildingSession<Options, Result>;
} // namespace lotus::cfl::classical::cat
