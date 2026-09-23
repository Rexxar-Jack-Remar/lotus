// SPDX-License-Identifier: MIT
// Independent implementation; not code copied from the authors' artifact.
// Dynamic kernel: Lei et al., PLDI 2024, Algorithm 3, DOI 10.1145/3656451.
// Static transformation: conservative subset, NOT all of Algorithm 2.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

namespace lotus::cfl::classical::skewed {

// Deliberately isolated from Lotus's public typedefs. Symbols are dense;
// vertex IDs need not be. All uint64_t vertex values are supported.
using Symbol = std::uint32_t;
using Node = std::uint64_t;

struct Rule {
  Symbol lhs = 0;
  std::uint8_t arity = 0;
  Symbol first = 0;
  Symbol second = 0;
  static Rule epsilon(Symbol x) { return {x, 0, 0, 0}; }
  static Rule unary(Symbol x, Symbol y) { return {x, 1, y, 0}; }
  static Rule binary(Symbol x, Symbol y, Symbol z) { return {x, 2, y, z}; }
  bool operator==(const Rule &r) const {
    return std::tie(lhs, arity, first, second) ==
           std::tie(r.lhs, r.arity, r.first, r.second);
  }
  bool operator<(const Rule &r) const {
    return std::tie(lhs, arity, first, second) <
           std::tie(r.lhs, r.arity, r.first, r.second);
  }
};

// Input MUST already be normalized: epsilon, unary, or binary productions.
// Terminal symbols may occur in either binary position. There is no parser,
// EBNF/attribute expansion, or grammar-specific implicit epsilon convention.
struct Grammar {
  Symbol start = 0;
  std::vector<bool> terminal;
  std::vector<Rule> rules;
  void validate() const;
};

struct Fact {
  Node source = 0;
  Symbol symbol = 0;
  Node target = 0;
  bool operator==(const Fact &f) const {
    return std::tie(symbol, source, target) ==
           std::tie(f.symbol, f.source, f.target);
  }
  bool operator<(const Fact &f) const {
    return std::tie(symbol, source, target) <
           std::tie(f.symbol, f.source, f.target);
  }
};

struct Graph {
  std::vector<Node> nodes; // Include isolated vertices here.
  std::vector<Fact> edges; // Terminal edges only; endpoints imply nodes.
};

enum class Scope { AllSymbols, TargetsOnly };
enum class StaticMode { Disabled, Conservative };

enum class RewriteKind {
  LeftRecursionTransfer,
  RightRecursionTransfer,
  TransitiveRecursionTransfer
};

struct Rewrite {
  RewriteKind kind;
  Symbol eliminated_recursion;
  std::vector<Symbol> consumers;
  std::size_t removed_rules = 0;
  std::size_t added_rules = 0;
  std::size_t epsilon_bases_removed = 0;
  std::size_t terminal_bases_inlined = 0;
};

struct Options {
  // AllSymbols is the safe default for Lotus's complete-triple contract.
  // It protects every nonterminal, so static rewriting has no eligible symbol.
  Scope scope = Scope::AllSymbols;
  StaticMode static_mode = StaticMode::Disabled;
  // Used only for TargetsOnly. Empty means {grammar.start}.
  std::vector<Symbol> targets;
  bool static_propagating_edges = true; // Algorithm 3, lines 13--14.
  bool dynamic_transitive_edges = true; // Algorithm 3, lines 15--16.
  // When present, use exactly this set as the statically propagating
  // nonterminals. Grammar adapters can map their Follow metadata here. An
  // empty vector explicitly disables static PE; nullopt retains structural
  // inference for grammars without that metadata.
  std::optional<std::vector<Symbol>> propagating_symbols;
  // Zero means unlimited. Counts E entries + PE entries, not output pairs.
  // Exceeding this limit throws; no incomplete Result is returned.
  std::size_t max_fact_entries = 0;
};

struct Stats {
  std::size_t nodes = 0;
  std::size_t unique_base_edges = 0;
  std::size_t indexed_facts = 0;
  std::size_t propagating_facts = 0;
  std::size_t output_facts = 0;
  std::size_t propagating_symbols = 0;
  std::size_t dynamic_eligible_symbols = 0;
  std::size_t static_pe_insertions = 0;
  std::size_t dynamic_pe_insertions = 0;
  std::size_t promotions_to_indexed = 0;
  std::size_t attempts = 0;
  std::size_t duplicate_attempts = 0;
  std::size_t unary_applications = 0;
  std::size_t binary_join_pairs = 0;
  std::size_t work_items = 0;
  std::size_t peak_worklist = 0;
};

struct Transformation {
  Grammar grammar;
  std::vector<Rewrite> rewrites;
};

// Languages of all symbols other than the recorded eliminated_recursion
// symbols are preserved. Selected targets are never changed. Conservative
// guards reject indirect cycles and mixed-position consumers.
Transformation skewConservatively(const Grammar &grammar,
                                  const std::vector<Symbol> &protected_symbols);

class ResourceLimit : public std::runtime_error {
public:
  explicit ResourceLimit(const std::string &s) : std::runtime_error(s) {}
};

class Result {
public:
  // Throws for a symbol outside the requested result scope. Unknown vertices
  // simply have no path. The zero-length path exists only for nullable symbols.
  bool contains(Node source, Symbol symbol, Node target) const;
  bool coversSymbol(Symbol symbol) const;
  const std::vector<Fact> &facts() const { return facts_; }
  const std::vector<Node> &nodes() const { return nodes_; }
  const std::vector<Symbol> &outputSymbols() const { return output_symbols_; }
  const Grammar &workingGrammar() const { return working_grammar_; }
  const Stats &stats() const { return stats_; }
  const std::vector<Rewrite> &rewrites() const { return rewrites_; }

private:
  friend Result solve(const Grammar &, const Graph &, const Options &);
  std::vector<Fact> facts_;
  std::vector<Node> nodes_;
  std::vector<Symbol> output_symbols_;
  Grammar working_grammar_;
  Stats stats_;
  std::vector<Rewrite> rewrites_;
};

// Batch solve. Neither input is modified. Validation failures and allocation /
// resource failures throw; there is no silent fallback or approximate answer.
Result solve(const Grammar &grammar, const Graph &graph,
             const Options &options = {});

// Monotone-update adapter. Each solve rebuilds from all terminal edges because
// the chosen PE policy requires old PE facts to see terminal edges added later.
// SolverSession owns the shared no-change check.
// Instances are not thread-safe; independent instances share no mutable state.
class RebuildingSession {
public:
  explicit RebuildingSession(Grammar grammar, Graph graph = {},
                             Options options = {});
  bool addNode(Node node);
  bool addTerminalEdge(Node source, Symbol symbol, Node target);
  void solve();
  const Result &result() const;
  const Graph &baseGraph() const { return graph_; }

private:
  Grammar grammar_;
  Graph graph_;
  Options options_;
  std::set<Node> known_nodes_;
  std::set<Fact> known_edges_;
  std::optional<Result> result_;
};

} // namespace lotus::cfl::classical::skewed
