// SPDX-License-Identifier: MIT
#pragma once
#include "CFL/Classical/Solvers/Engines/Skewed/SkewedTabulation.h"

#include <limits>
#include <unordered_map>
#include <utility>

namespace lotus::cfl::classical::skewed {

// Header-only import/export boundary. ExternalSymbol may be a Lotus SymbolId
// or std::string; IDs need not be dense. This does NOT assume unverified Lotus
// accessor names. Feed it from Lotus's normalized grammar / base graph
// visitors.
template <class ExternalSymbol, class Hash = std::hash<ExternalSymbol>>
class InputBridge {
public:
  Symbol declareSymbol(const ExternalSymbol &external, bool terminal) {
    auto found = ids_.find(external);
    if (found != ids_.end()) {
      if (grammar_.terminal[found->second] != terminal)
        throw std::invalid_argument("inconsistent terminal declaration");
      return found->second;
    }
    if (symbols_.size() >= std::numeric_limits<Symbol>::max())
      throw ResourceLimit("too many imported symbols");
    const auto id = static_cast<Symbol>(symbols_.size());
    // Roll back all components if an allocation or key copy fails.
    symbols_.push_back(external);
    try {
      grammar_.terminal.push_back(terminal);
    } catch (...) {
      symbols_.pop_back();
      throw;
    }
    try {
      ids_.emplace(external, id);
    } catch (...) {
      grammar_.terminal.pop_back();
      symbols_.pop_back();
      throw;
    }
    return id;
  }
  Symbol idOf(const ExternalSymbol &external) const {
    auto it = ids_.find(external);
    if (it == ids_.end())
      throw std::invalid_argument("undeclared imported symbol");
    return it->second;
  }
  const ExternalSymbol &externalSymbol(Symbol id) const {
    return symbols_.at(id);
  }
  void setStart(const ExternalSymbol &s) {
    const Symbol id = idOf(s);
    if (grammar_.terminal[id])
      throw std::invalid_argument("terminal start symbol");
    grammar_.start = id;
    start_set_ = true;
  }
  void addEpsilon(const ExternalSymbol &x) {
    grammar_.rules.push_back(Rule::epsilon(idOf(x)));
  }
  void addUnary(const ExternalSymbol &x, const ExternalSymbol &y) {
    grammar_.rules.push_back(Rule::unary(idOf(x), idOf(y)));
  }
  void addBinary(const ExternalSymbol &x, const ExternalSymbol &y,
                 const ExternalSymbol &z) {
    grammar_.rules.push_back(Rule::binary(idOf(x), idOf(y), idOf(z)));
  }
  void addNode(Node v) { graph_.nodes.push_back(v); }
  void addTerminalEdge(Node u, const ExternalSymbol &s, Node v) {
    Symbol id = idOf(s);
    if (!grammar_.terminal[id])
      throw std::invalid_argument("nonterminal base edge");
    graph_.edges.push_back({u, id, v});
  }
  const Grammar &grammar() const {
    if (!start_set_)
      throw std::logic_error("setStart() required");
    return grammar_;
  }
  const Graph &graph() const { return graph_; }
  Result solve(const Options &o = {}) const {
    return skewed::solve(grammar(), graph_, o);
  }
  template <class Sink> void exportFacts(const Result &r, Sink &&sink) const {
    // r must come from this bridge's symbol mapping.
    for (const auto &f : r.facts())
      sink(f.source, externalSymbol(f.symbol), f.target);
  }

private:
  Grammar grammar_;
  Graph graph_;
  std::vector<ExternalSymbol> symbols_;
  std::unordered_map<ExternalSymbol, Symbol, Hash> ids_;
  bool start_set_ = false;
};
} // namespace lotus::cfl::classical::skewed
