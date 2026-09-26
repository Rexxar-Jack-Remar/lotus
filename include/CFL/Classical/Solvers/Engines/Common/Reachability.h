// SPDX-License-Identifier: MIT
#pragma once
#include "CFL/Classical/Solvers/Engines/Skewed/SkewedTabulation.h"

#include <functional>
#include <map>
#include <utility>

namespace lotus::cfl::classical::common {
// Same input vocabulary as the previously delivered Skewed engine. No import
// or conversion is necessary between the three solvers.
using skewed::Fact;
using skewed::Grammar;
using skewed::Graph;
using skewed::Node;
using skewed::ResourceLimit;
using skewed::Rule;
using skewed::Scope;
using skewed::Symbol;

struct Query {
  Scope scope = Scope::TargetsOnly;
  std::vector<Symbol> targets; // Empty => the start symbol, unless AllSymbols.
};
struct Limits {
  // Limits count successful insertions cumulatively, popped work items, and
  // stored result triples respectively. Zero means unlimited. Exceptions
  // discard the current solve; no partial solution is published.
  std::size_t max_insertions = 0;
  std::size_t max_work_items = 0;
  std::size_t max_result_entries = 0;
  std::size_t max_epochs = 0;
};

// Exact on requested symbols only. Nonterminal pairs may be stored over SCC
// representatives. Terminal answers ALWAYS use original (uncontracted) edges.
// Materialization is explicit: one compressed pair can represent O(|V|^2)
// original-node pairs. contains() does not expand an equivalence class.
class Reachability {
public:
  bool coversSymbol(Symbol s) const;
  bool contains(Node u, Symbol s, Node v) const;
  const std::vector<Node> &nodes() const { return nodes_; }
  const std::vector<Symbol> &outputSymbols() const { return outputs_; }
  const std::vector<Fact> &quotientFacts() const { return facts_; }
  const std::vector<Fact> &originalTerminalFacts() const { return terminals_; }
  // Identity representatives for uncontracted results. Unknown node => nullopt.
  std::optional<Node> representative(Node node) const;
  const std::vector<Node> &representatives() const { return reps_; }
  // Count overflows throw rather than wrap. max_facts is checked BEFORE
  // invoking any callback, so an export limit cannot leave a half-filled
  // destination.
  std::size_t expandedSize() const;
  std::size_t expandedSize(Symbol symbol) const;
  bool visitFacts(Symbol symbol,
                  const std::function<bool(const Fact &)> &visitor) const;
  bool visitSuccessors(Node source, Symbol symbol,
                       const std::function<bool(Node)> &visitor) const;
  bool visitPredecessors(Symbol symbol, Node target,
                         const std::function<bool(Node)> &visitor) const;
  void forEachFact(const std::function<void(const Fact &)> &sink,
                   std::size_t max_facts = 0) const;
  std::vector<Fact> materialize(std::size_t max_facts = 0) const;

  // Internal construction boundary, also useful to adapters. nodes and reps
  // have equal size; nodes are sorted unique; facts use representatives. The
  // solver verifies that every requested nonterminal is invariant under reps.
  static Reachability build(std::vector<Node> nodes, std::vector<Node> reps,
                            std::vector<Symbol> outputs,
                            std::vector<bool> terminal, std::vector<Fact> facts,
                            std::vector<Fact> terminals, const Limits &limits);

private:
  std::vector<Node> nodes_, reps_;
  std::vector<Symbol> outputs_;
  std::vector<bool> terminal_;
  std::vector<Fact> facts_, terminals_;
  std::map<Node, std::vector<Node>> members_;
};

// Safe update wrapper for either engine. Offline CAT annotations and quotient
// graphs are invalidated by base-graph changes. Updates rebuild, never silently
// reuse a stale context/partition. Instance-local state; concurrent independent
// instances are supported. The Result type is inferred from the solve function.
template <class Options, class Result> class RebuildingSession {
public:
  using Solve = Result (*)(const Grammar &, const Graph &, const Options &);
  RebuildingSession(Grammar grammar, Graph graph, Options options, Solve solver)
      : grammar_(std::move(grammar)), graph_(std::move(graph)),
        options_(std::move(options)), solver_(solver) {
    grammar_.validate();
    if (!solver_)
      throw std::invalid_argument("null solver");
    for (Node n : graph_.nodes)
      nodes_.insert(n);
    for (const auto &f : graph_.edges) {
      checkTerminal(f.symbol);
      edges_.insert(f);
      nodes_.insert(f.source);
      nodes_.insert(f.target);
    }
  }
  bool addNode(Node n) {
    auto p = nodes_.insert(n);
    if (!p.second)
      return false;
    try {
      graph_.nodes.push_back(n);
    } catch (...) {
      nodes_.erase(p.first);
      throw;
    }
    dirty_ = true;
    return true;
  }
  bool addTerminalEdge(Node u, Symbol s, Node v) {
    checkTerminal(s);
    Fact f{u, s, v};
    if (edges_.count(f))
      return false;
    bool eu = false, ev = false, ee = false;
    try {
      eu = nodes_.insert(u).second;
      ev = nodes_.insert(v).second;
      ee = edges_.insert(f).second;
      graph_.edges.push_back(f);
    } catch (...) {
      if (ee)
        edges_.erase(f);
      if (ev)
        nodes_.erase(v);
      if (eu)
        nodes_.erase(u);
      throw;
    }
    dirty_ = true;
    return true;
  }
  bool solve() {
    if (!dirty_)
      return false;
    Result next = solver_(grammar_, graph_, options_);
    result_ = std::move(next);
    dirty_ = false;
    return true;
  }
  bool dirty() const { return dirty_; }
  const Result &result() const {
    if (dirty_ || !result_)
      throw std::logic_error("solve() required before querying");
    return *result_;
  }
  const Graph &baseGraph() const { return graph_; }

private:
  void checkTerminal(Symbol s) const {
    if (s >= grammar_.terminal.size() || !grammar_.terminal[s])
      throw std::invalid_argument(
          "base edges require declared terminal symbols");
  }
  Grammar grammar_;
  Graph graph_;
  Options options_;
  Solve solver_;
  std::set<Node> nodes_;
  std::set<Fact> edges_;
  std::optional<Result> result_;
  bool dirty_ = true;
};
} // namespace lotus::cfl::classical::common
