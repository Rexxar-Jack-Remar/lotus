#include "CFL/Classical/Solvers/Engines/EndpointQuotient/EndpointQuotientEngine.h"

#include "CFL/Classical/Solvers/Engines/EndpointQuotient/EndpointQuotient.h"

#include <algorithm>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace lotus::cfl::classical::engines {
namespace {

using endpoint::Id;

struct EdgeKey {
  SymbolId symbol;
  NodeId source;
  NodeId target;

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

} // namespace

class EndpointQuotientEngine::Impl {
public:
  Impl(const Grammar &grammar, std::size_t node_count, bool factorized)
      : grammar_(grammar), node_count_(node_count) {
    options_.factorized = factorized;
    buildRules(base_);
    base_.symbols = grammar.symbolCount();
    for (const auto &symbol : grammar.countSymbols())
      count_symbols_.push_back(grammar.symbolId(symbol));
    std::sort(count_symbols_.begin(), count_symbols_.end());
  }

  void ensureNodeCount(std::size_t node_count) {
    if (node_count > node_count_) {
      node_count_ = node_count;
      dirty_ = true;
    }
  }

  bool add(SymbolId symbol, NodeId source, NodeId target) {
    if (symbol >= base_.symbols || source >= node_count_ ||
        target >= node_count_)
      throw std::out_of_range("Endpoint quotient input ID out of range");
    const bool added = edges_.emplace(EdgeKey{symbol, source, target}).second;
    dirty_ = dirty_ || added;
    return added;
  }

  EndpointQuotientStatistics solve() {
    if (!dirty_) {
      // Cardinalities describe the snapshot; operation counters describe this
      // call.
      auto result = stats_;
      result.derived_facts = 0;
      result.binary_joins = result.bridge_pairs = result.worklist_pops = 0;
      result.peak_worklist = result.insert_attempts = result.duplicate_facts =
          0;
      result.binary_propagations = result.successful_binary_propagations = 0;
      result.repeated_binary_outputs = result.partitions_built = 0;
      result.binary_join_words = 0;
      result.bridges_built = result.lifts_built = 0;
      result.preprocess_us = result.saturation_us = result.count_us = 0;
      stats_ = result;
      return stats_;
    }
    endpoint::Problem problem = base_;
    problem.nodes = node_count_;
    problem.edges.reserve(edges_.size());
    for (const EdgeKey &edge : edges_)
      problem.edges.push_back({edge.source, edge.symbol, edge.target});

    auto next =
        snapshot_
            ? std::make_unique<endpoint::Solver>(std::move(problem), *snapshot_,
                                                 options_)
            : std::make_unique<endpoint::Solver>(std::move(problem), options_);
    next->solve();
    const auto count = next->countOffDiagonalUnion(
        std::vector<Id>(count_symbols_.begin(), count_symbols_.end()));
    auto result = collect(next->statistics());
    const auto previous = snapshot_ ? snapshot_->statistics().logical_facts : 0;
    result.derived_facts = result.logical_facts - previous;
    snapshot_ = std::move(next);
    count_symbol_edges_ = count;
    dirty_ = false;
    stats_ = result;
    return stats_;
  }

  bool contains(SymbolId symbol, NodeId source, NodeId target) const {
    return snapshot_ && symbol < base_.symbols &&
           source < snapshot_->nodeCount() && target < snapshot_->nodeCount() &&
           snapshot_->contains(symbol, source, target);
  }

  bool visitSuccessors(SymbolId symbol, NodeId source,
                       NodeVisitor visitor) const {
    if (source >= node_count_)
      throw std::out_of_range("Endpoint quotient source out of range");
    return !snapshot_ || symbol >= base_.symbols ||
           source >= snapshot_->nodeCount() ||
           snapshot_->visitSuccessors(symbol, source, visitor);
  }

  bool visitPredecessors(SymbolId symbol, NodeId target,
                         NodeVisitor visitor) const {
    if (target >= node_count_)
      throw std::out_of_range("Endpoint quotient target out of range");
    return !snapshot_ || symbol >= base_.symbols ||
           target >= snapshot_->nodeCount() ||
           snapshot_->visitPredecessors(symbol, target, visitor);
  }

  bool visitEdges(SymbolId symbol, EdgeVisitor visitor) const {
    return !snapshot_ || symbol >= base_.symbols ||
           snapshot_->visitFacts(symbol, [&](Id source, Id target) {
             return visitor({symbol, source, target});
           });
  }

  bool visitEdges(EdgeVisitor visitor) const {
    for (Id a = 0; a < base_.symbols; ++a)
      if (!visitEdges(static_cast<SymbolId>(a), visitor))
        return false;
    return true;
  }

  std::size_t edgeCount() const {
    return snapshot_ ? snapshot_->statistics().logical_facts : 0;
  }

  std::size_t edgeCount(SymbolId symbol) const {
    return snapshot_ && symbol < base_.symbols
               ? snapshot_->statistics().per_symbol[symbol].logical_facts
               : 0;
  }

  std::size_t estimatedPayloadBytes() const {
    std::size_t bytes = sizeof(EndpointQuotientEngine) + sizeof(Impl) +
                        base_.rules.capacity() * sizeof(endpoint::Rule) +
                        edges_.size() * sizeof(EdgeKey) +
                        edges_.bucket_count() * sizeof(void *);
    if (snapshot_)
      bytes += snapshot_->estimatedPayloadBytes();
    bytes += count_symbols_.capacity() * sizeof(SymbolId);
    return bytes;
  }

  std::size_t countOffDiagonalUnion(std::vector<SymbolId> symbols) const {
    if (!snapshot_)
      return 0;
    std::sort(symbols.begin(), symbols.end());
    symbols.erase(std::unique(symbols.begin(), symbols.end()), symbols.end());
    if (symbols == count_symbols_)
      return count_symbol_edges_;
    return snapshot_->countOffDiagonalUnion(
        std::vector<Id>(symbols.begin(), symbols.end()));
  }

  const EndpointQuotientStatistics &statistics() const { return stats_; }

private:
  void buildRules(endpoint::Problem &problem) const {
    for (const auto &[head, rules] : grammar_.productions()) {
      const Id lhs = grammar_.symbolId(head);
      for (const auto &rule : rules) {
        if (rule.empty()) {
          throw std::logic_error(
              "Endpoint quotient requires a normalized grammar");
        }
        if (rule.size() == 1 && rule.front() == Grammar::kEpsilonSymbol) {
          problem.rules.push_back(endpoint::Rule::epsilon(lhs));
          continue;
        }
        if (rule.size() == 1) {
          problem.rules.push_back(
              endpoint::Rule::unary(lhs, grammar_.symbolId(rule.front())));
          continue;
        }
        if (rule.size() == 2) {
          problem.rules.push_back(endpoint::Rule::binary(
              lhs, grammar_.symbolId(rule[0]), grammar_.symbolId(rule[1])));
          continue;
        }
        throw std::logic_error(
            "Endpoint quotient requires a normalized (binarized) grammar");
      }
    }
  }

  static EndpointQuotientStatistics collect(const endpoint::Statistics &eq) {
    EndpointQuotientStatistics result;
    result.cells = eq.cells;
    result.logical_facts = eq.logical_facts;
    result.seed_facts = eq.seed_facts;
    result.inferred_facts = eq.inferred_facts;
    result.binary_joins = eq.binary_joins;
    result.bridge_pairs = eq.bridge_pairs;
    result.nullable_symbols = eq.nullable_symbols;
    result.worklist_pops = eq.worklist_pops;
    result.peak_worklist = eq.peak_worklist;
    result.duplicate_facts = eq.duplicate_inserts;
    result.insert_attempts = eq.insert_attempts;
    result.binary_propagations = eq.binary_propagations;
    result.successful_binary_propagations = eq.successful_binary_propagations;
    result.repeated_binary_outputs = eq.repeated_binary_outputs;
    result.binary_join_words = eq.binary_join_words;
    result.partitions_built = eq.partitions_built;
    result.bridges_built = eq.bridges_built;
    result.lifts_built = eq.lifts_built;
    for (const auto &symbol : eq.per_symbol) {
      result.source_classes += symbol.source_classes;
      result.target_classes += symbol.target_classes;
    }
    result.preprocess_us = static_cast<std::uint64_t>(eq.preprocess_ms * 1000);
    result.saturation_us = static_cast<std::uint64_t>(eq.saturation_ms * 1000);
    result.count_us = static_cast<std::uint64_t>(eq.count_ms * 1000);
    return result;
  }

  const Grammar &grammar_;
  endpoint::Options options_;
  endpoint::Problem base_;
  std::unique_ptr<endpoint::Solver> snapshot_;
  bool dirty_ = true;
  std::vector<SymbolId> count_symbols_;
  std::size_t count_symbol_edges_ = 0;
  std::size_t node_count_ = 0;
  std::unordered_set<EdgeKey, EdgeKeyHash> edges_;
  EndpointQuotientStatistics stats_;
};

EndpointQuotientEngine::EndpointQuotientEngine(const Grammar &grammar,
                                               std::size_t node_count,
                                               bool factorized)
    : impl_(new Impl(grammar, node_count, factorized)) {}

EndpointQuotientEngine::~EndpointQuotientEngine() = default;

void EndpointQuotientEngine::ensureNodeCount(std::size_t node_count) {
  impl_->ensureNodeCount(node_count);
}

bool EndpointQuotientEngine::add(SymbolId symbol, NodeId source,
                                 NodeId target) {
  return impl_->add(symbol, source, target);
}

EndpointQuotientStatistics EndpointQuotientEngine::solve() {
  return impl_->solve();
}

const EndpointQuotientStatistics &EndpointQuotientEngine::statistics() const {
  return impl_->statistics();
}

bool EndpointQuotientEngine::contains(SymbolId symbol, NodeId source,
                                      NodeId target) const {
  return impl_->contains(symbol, source, target);
}

bool EndpointQuotientEngine::visitSuccessors(SymbolId symbol, NodeId source,
                                             NodeVisitor visitor) const {
  return impl_->visitSuccessors(symbol, source, visitor);
}

bool EndpointQuotientEngine::visitPredecessors(SymbolId symbol, NodeId target,
                                               NodeVisitor visitor) const {
  return impl_->visitPredecessors(symbol, target, visitor);
}

bool EndpointQuotientEngine::visitEdges(EdgeVisitor visitor) const {
  return impl_->visitEdges(visitor);
}

bool EndpointQuotientEngine::visitEdges(SymbolId symbol,
                                        EdgeVisitor visitor) const {
  return impl_->visitEdges(symbol, visitor);
}

std::size_t EndpointQuotientEngine::edgeCount() const {
  return impl_->edgeCount();
}

std::size_t EndpointQuotientEngine::edgeCount(SymbolId symbol) const {
  return impl_->edgeCount(symbol);
}

std::size_t EndpointQuotientEngine::estimatedPayloadBytes() const {
  return impl_->estimatedPayloadBytes();
}

std::size_t EndpointQuotientEngine::countOffDiagonalUnion(
    std::vector<SymbolId> symbols) const {
  return impl_->countOffDiagonalUnion(std::move(symbols));
}

} // namespace lotus::cfl::classical::engines
