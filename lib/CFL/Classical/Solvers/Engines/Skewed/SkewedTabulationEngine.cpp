#include "CFL/Classical/Solvers/Engines/Skewed/SkewedTabulationEngine.h"

#include <algorithm>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <unordered_set>
#include <utility>
#include <vector>

namespace lotus::cfl::classical::engines {
namespace {

struct EdgeKey {
  SymbolId symbol = 0;
  NodeId source = 0;
  NodeId target = 0;

  bool operator==(const EdgeKey &other) const {
    return symbol == other.symbol && source == other.source &&
           target == other.target;
  }
};

struct EdgeKeyHash {
  std::size_t operator()(const EdgeKey &key) const noexcept {
    std::size_t hash = std::hash<SymbolId>{}(key.symbol);
    hash ^= std::hash<NodeId>{}(key.source) + 0x9e3779b9U + (hash << 6) +
            (hash >> 2);
    hash ^= std::hash<NodeId>{}(key.target) + 0x9e3779b9U + (hash << 6) +
            (hash >> 2);
    return hash;
  }
};

struct ImportedGrammar {
  skewed::Grammar grammar;
  std::vector<skewed::Symbol> seed_symbols;
  std::size_t original_symbols = 0;
};

ImportedGrammar importGrammar(const Grammar &grammar) {
  ImportedGrammar result;
  result.original_symbols = grammar.symbolCount();
  result.grammar.start = grammar.startSymbolId();
  result.grammar.terminal.resize(grammar.symbolCount());
  result.seed_symbols.resize(grammar.symbolCount());

  std::size_t nonterminals = 0;
  for (SymbolId symbol = 0; symbol < grammar.symbolCount(); ++symbol) {
    const bool terminal = grammar.isTerminal(grammar.symbolName(symbol));
    result.grammar.terminal[symbol] = terminal;
    nonterminals += terminal ? 0 : 1;
  }
  const std::size_t max_symbols = std::numeric_limits<skewed::Symbol>::max();
  if (grammar.symbolCount() > max_symbols ||
      nonterminals > max_symbols - grammar.symbolCount()) {
    throw skewed::ResourceLimit(
        "too many symbols for skewed-tabulation import");
  }

  for (const auto &[head, rules] : grammar.productions()) {
    const auto lhs = grammar.symbolId(head);
    for (const auto &rule : rules) {
      if (rule.size() == 1 && rule.front() == Grammar::kEpsilonSymbol) {
        result.grammar.rules.push_back(skewed::Rule::epsilon(lhs));
      } else if (rule.size() == 1) {
        result.grammar.rules.push_back(
            skewed::Rule::unary(lhs, grammar.symbolId(rule.front())));
      } else if (rule.size() == 2) {
        result.grammar.rules.push_back(skewed::Rule::binary(
            lhs, grammar.symbolId(rule[0]), grammar.symbolId(rule[1])));
      } else {
        throw std::logic_error(
            "Skewed tabulation requires a normalized grammar");
      }
    }
  }

  // SolverSession can migrate already-derived nonterminal facts while a
  // client extends its grammar. Represent each such axiom by a private
  // terminal plus a unary production, keeping the standalone kernel's input
  // contract terminal-only without exposing the private symbols in Relation.
  for (SymbolId symbol = 0; symbol < grammar.symbolCount(); ++symbol) {
    if (result.grammar.terminal[symbol]) {
      result.seed_symbols[symbol] = symbol;
      continue;
    }
    const auto seed =
        static_cast<skewed::Symbol>(result.grammar.terminal.size());
    result.grammar.terminal.push_back(true);
    result.grammar.rules.push_back(skewed::Rule::unary(symbol, seed));
    result.seed_symbols[symbol] = seed;
  }
  result.grammar.validate();
  return result;
}

skewed::Graph makeGraph(std::size_t node_count) {
  static_assert(sizeof(NodeId) <= sizeof(skewed::Node),
                "Lotus node IDs must fit skewed node IDs");
  skewed::Graph graph;
  graph.nodes.resize(node_count);
  std::iota(graph.nodes.begin(), graph.nodes.end(), skewed::Node{0});
  return graph;
}

} // namespace

class SkewedTabulationEngine::Impl {
public:
  Impl(const Grammar &grammar, std::size_t node_count, skewed::Options options)
      : imported_(importGrammar(grammar)),
        session_(imported_.grammar, makeGraph(node_count), std::move(options)),
        snapshot_(
            createRelation(RelationBackend::SparseBitVectors, node_count)),
        node_count_(node_count) {}

  void ensureNodeCount(std::size_t node_count) {
    if (node_count <= node_count_) {
      return;
    }
    for (NodeId node = node_count_; node < node_count; ++node) {
      session_.addNode(node);
    }
    snapshot_->ensureNodeCount(node_count);
    node_count_ = node_count;
  }

  bool add(SymbolId symbol, NodeId source, NodeId target) {
    if (symbol >= imported_.original_symbols || source >= node_count_ ||
        target >= node_count_) {
      throw std::out_of_range("Skewed-tabulation input ID out of range");
    }
    const EdgeKey key{symbol, source, target};
    const auto inserted = pending_inputs_.insert(key);
    try {
      if (!session_.addTerminalEdge(source, imported_.seed_symbols[symbol],
                                    target)) {
        if (inserted.second) {
          pending_inputs_.erase(inserted.first);
        }
        return false;
      }
    } catch (...) {
      if (inserted.second) {
        pending_inputs_.erase(inserted.first);
      }
      throw;
    }
    return true;
  }

  SkewedTabulationStatistics solve() {
    session_.solve();

    auto next = createRelation(RelationBackend::SparseBitVectors, node_count_);
    std::size_t derived_facts = 0;
    for (const skewed::Fact &fact : session_.result().facts()) {
      if (fact.symbol >= imported_.original_symbols) {
        continue;
      }
      const EdgeKey key{fact.symbol, static_cast<NodeId>(fact.source),
                        static_cast<NodeId>(fact.target)};
      if (!snapshot_->contains(key.symbol, key.source, key.target) &&
          pending_inputs_.count(key) == 0) {
        ++derived_facts;
      }
      next->add(key.symbol, key.source, key.target);
    }

    SkewedTabulationStatistics next_stats;
    static_cast<skewed::Stats &>(next_stats) = session_.result().stats();
    next_stats.derived_facts = derived_facts;
    snapshot_ = std::move(next);
    pending_inputs_.clear();
    stats_ = next_stats;
    return stats_;
  }

  bool contains(SymbolId symbol, NodeId source, NodeId target) const {
    return symbol < imported_.original_symbols && source < node_count_ &&
           target < node_count_ && snapshot_->contains(symbol, source, target);
  }

  bool visitSuccessors(SymbolId symbol, NodeId source,
                       NodeVisitor visitor) const {
    if (source >= node_count_) {
      throw std::out_of_range("Skewed-tabulation source out of range");
    }
    return symbol >= imported_.original_symbols ||
           snapshot_->visitSuccessors(symbol, source, visitor);
  }

  bool visitPredecessors(SymbolId symbol, NodeId target,
                         NodeVisitor visitor) const {
    if (target >= node_count_) {
      throw std::out_of_range("Skewed-tabulation target out of range");
    }
    return symbol >= imported_.original_symbols ||
           snapshot_->visitPredecessors(symbol, target, visitor);
  }

  bool visitEdges(EdgeVisitor visitor) const {
    return snapshot_->visitEdges(visitor);
  }

  bool visitEdges(SymbolId symbol, EdgeVisitor visitor) const {
    return symbol >= imported_.original_symbols ||
           snapshot_->visitEdges(symbol, visitor);
  }

  std::size_t edgeCount() const { return snapshot_->edgeCount(); }

  std::size_t edgeCount(SymbolId symbol) const {
    return symbol < imported_.original_symbols ? snapshot_->edgeCount(symbol)
                                               : 0;
  }

  std::size_t estimatedPayloadBytes() const {
    return sizeof(Impl) + snapshot_->estimatedPayloadBytes() +
           imported_.grammar.terminal.capacity() * sizeof(bool) +
           imported_.grammar.rules.capacity() * sizeof(skewed::Rule) +
           imported_.seed_symbols.capacity() * sizeof(skewed::Symbol) +
           pending_inputs_.size() * sizeof(EdgeKey) +
           pending_inputs_.bucket_count() * sizeof(void *);
  }

  const SkewedTabulationStatistics &statistics() const { return stats_; }

private:
  ImportedGrammar imported_;
  skewed::RebuildingSession session_;
  std::unique_ptr<Relation> snapshot_;
  std::unordered_set<EdgeKey, EdgeKeyHash> pending_inputs_;
  std::size_t node_count_ = 0;
  SkewedTabulationStatistics stats_;
};

SkewedTabulationEngine::SkewedTabulationEngine(const Grammar &grammar,
                                               std::size_t node_count,
                                               skewed::Options options)
    : impl_(std::make_unique<Impl>(grammar, node_count, std::move(options))) {}

SkewedTabulationEngine::~SkewedTabulationEngine() = default;

void SkewedTabulationEngine::ensureNodeCount(std::size_t node_count) {
  impl_->ensureNodeCount(node_count);
}

bool SkewedTabulationEngine::add(SymbolId symbol, NodeId source,
                                 NodeId target) {
  return impl_->add(symbol, source, target);
}

SkewedTabulationStatistics SkewedTabulationEngine::solve() {
  return impl_->solve();
}

const SkewedTabulationStatistics &SkewedTabulationEngine::statistics() const {
  return impl_->statistics();
}

bool SkewedTabulationEngine::contains(SymbolId symbol, NodeId source,
                                      NodeId target) const {
  return impl_->contains(symbol, source, target);
}

bool SkewedTabulationEngine::visitSuccessors(SymbolId symbol, NodeId source,
                                             NodeVisitor visitor) const {
  return impl_->visitSuccessors(symbol, source, visitor);
}

bool SkewedTabulationEngine::visitPredecessors(SymbolId symbol, NodeId target,
                                               NodeVisitor visitor) const {
  return impl_->visitPredecessors(symbol, target, visitor);
}

bool SkewedTabulationEngine::visitEdges(EdgeVisitor visitor) const {
  return impl_->visitEdges(visitor);
}

bool SkewedTabulationEngine::visitEdges(SymbolId symbol,
                                        EdgeVisitor visitor) const {
  return impl_->visitEdges(symbol, visitor);
}

std::size_t SkewedTabulationEngine::edgeCount() const {
  return impl_->edgeCount();
}

std::size_t SkewedTabulationEngine::edgeCount(SymbolId symbol) const {
  return impl_->edgeCount(symbol);
}

std::size_t SkewedTabulationEngine::estimatedPayloadBytes() const {
  return impl_->estimatedPayloadBytes();
}

} // namespace lotus::cfl::classical::engines
