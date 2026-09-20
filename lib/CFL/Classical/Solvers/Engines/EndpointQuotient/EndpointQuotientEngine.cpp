#include "CFL/Classical/Solvers/Engines/EndpointQuotient/EndpointQuotientEngine.h"

#include "CFL/Classical/Solvers/Engines/EndpointQuotient/EndpointQuotient.h"
#include "CFL/Classical/Solvers/Engines/EndpointQuotient/EndpointQuotientStaging.h"

#include <algorithm>
#include <stdexcept>
#include <tuple>
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

static bool edgeKeyLess(const EdgeKey &lhs, const EdgeKey &rhs) noexcept {
  return std::tie(lhs.symbol, lhs.source, lhs.target) <
         std::tie(rhs.symbol, rhs.source, rhs.target);
}

/// Monotone terminal input with a compact immutable prefix and a hash-indexed
/// delta. Keeping every historical edge in an unordered_set made the engine
/// retain the hash-node and bucket overhead for the entire session, even
/// though only the edges added since the previous solve need O(1) lookup.
///
/// commit() is called only after the new solver snapshot and its derived counts
/// have completed, so a throwing solve leaves both the last queryable snapshot
/// and the complete buffered input unchanged.
class MonotoneEdgeStore {
public:
  bool add(EdgeKey edge) {
    if (std::binary_search(committed_.begin(), committed_.end(), edge,
                           edgeKeyLess))
      return false;
    return pending_.emplace(std::move(edge)).second;
  }

  void appendTo(std::vector<endpoint::Edge> &result) const {
    result.reserve(result.size() + size());
    for (const EdgeKey &edge : committed_)
      result.push_back({edge.source, edge.symbol, edge.target});
    for (const EdgeKey &edge : pending_)
      result.push_back({edge.source, edge.symbol, edge.target});
  }

  void commit() {
    // Complete every potentially-throwing allocation before modifying the
    // committed prefix. The merge itself uses the reserved tail as output and
    // only no-throw assignments of this trivial key type.
    std::vector<EdgeKey> delta;
    delta.reserve(pending_.size());
    decltype(pending_) empty_pending;
    for (const EdgeKey &edge : pending_)
      delta.push_back(edge);
    std::sort(delta.begin(), delta.end(), edgeKeyLess);
    const std::size_t old_size = committed_.size();
    const std::size_t new_size = old_size + delta.size();
    committed_.reserve(new_size);
    committed_.resize(new_size);
    // committed_ is sorted. Sorting only the usually-small delta followed by
    // a backwards merge avoids re-sorting the historical input every round.
    std::size_t left = old_size, right = delta.size(), output = new_size;
    while (left && right) {
      if (edgeKeyLess(delta[right - 1], committed_[left - 1]))
        committed_[--output] = committed_[--left];
      else
        committed_[--output] = delta[--right];
    }
    while (right)
      committed_[--output] = delta[--right];
    // A callgraph round can temporarily make the delta table large. Release
    // its buckets after commit rather than retaining that peak for the rest of
    // the analysis.
    pending_.swap(empty_pending);
  }

  std::size_t size() const { return committed_.size() + pending_.size(); }

  std::size_t payloadBytes() const {
    return committed_.capacity() * sizeof(EdgeKey) +
           pending_.size() * sizeof(EdgeKey) +
           pending_.bucket_count() * sizeof(void *);
  }

private:
  std::vector<EdgeKey> committed_;
  std::unordered_set<EdgeKey, EdgeKeyHash> pending_;
};

} // namespace

class EndpointQuotientEngine::Impl {
public:
  Impl(const Grammar &grammar, std::size_t node_count, bool factorized)
      : grammar_(grammar), node_count_(node_count) {
    options_.factorized = factorized;
    buildRules(base_);
    base_.symbols = grammar.symbolCount();
    buildSccMetadata();
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
    const bool added = edges_.add(EdgeKey{symbol, source, target});
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
      result.hottest_rule_joins = result.hottest_scc_joins = 0;
      result.hottest_rule_id = result.hottest_scc_id = 0;
      for (auto &rule : result.per_rule)
        clearWork(rule);
      for (auto &scc : result.per_scc)
        clearWork(scc);
      stats_ = result;
      return stats_;
    }
    endpoint::Problem problem = base_;
    problem.nodes = node_count_;
    edges_.appendTo(problem.edges);

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
    // Commit input and result together. Everything above may throw, in which
    // case the old snapshot and the pending delta remain available for query
    // and retry with the same externally visible state as before this call.
    edges_.commit();
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
                        edges_.payloadBytes();
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
  static void clearWork(EndpointQuotientRuleStatistics &profile) {
    profile.delta_rows = profile.delta_cells = profile.joins = 0;
    profile.propagations = profile.successful_propagations = 0;
    profile.repeated_outputs = profile.join_word_operations = 0;
  }

  static void clearWork(EndpointQuotientSccStatistics &profile) {
    profile.delta_rows = profile.delta_cells = profile.joins = 0;
    profile.propagations = profile.successful_propagations = 0;
    profile.repeated_outputs = profile.join_word_operations = 0;
  }

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

  void buildSccMetadata() {
    const endpoint::StagingPlan plan = endpoint::buildStagingPlan(base_);
    symbol_scc_ = plan.symbol_to_stage;
    scc_metadata_.resize(plan.stages.size());
    for (const auto &stage : plan.stages) {
      auto &profile = scc_metadata_[stage.index];
      profile.scc_id = stage.index;
      profile.symbols = stage.symbols.size();
      profile.rules = stage.rules.size();
      switch (stage.kind) {
      case endpoint::SccKind::Acyclic:
        profile.classification = EndpointQuotientSccClass::Acyclic;
        break;
      case endpoint::SccKind::UnaryRegular:
        profile.classification = EndpointQuotientSccClass::UnaryRecursive;
        break;
      case endpoint::SccKind::LeftLinear:
        profile.classification = EndpointQuotientSccClass::LeftLinear;
        break;
      case endpoint::SccKind::RightLinear:
        profile.classification = EndpointQuotientSccClass::RightLinear;
        break;
      case endpoint::SccKind::TransitiveSelf:
        profile.classification = EndpointQuotientSccClass::Transitive;
        break;
      case endpoint::SccKind::General:
        profile.classification = EndpointQuotientSccClass::General;
        break;
      }
    }
    rule_scc_.resize(base_.rules.size());
    for (Id rule_id = 0; rule_id < base_.rules.size(); ++rule_id)
      rule_scc_[rule_id] = symbol_scc_[base_.rules[rule_id].lhs];
  }

  EndpointQuotientStatistics collect(const endpoint::Statistics &eq) const {
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

    result.per_rule.reserve(base_.rules.size());
    result.per_scc = scc_metadata_;
    for (Id rule_id = 0; rule_id < base_.rules.size(); ++rule_id) {
      const auto &rule = base_.rules[rule_id];
      EndpointQuotientRuleStatistics profile;
      profile.rule_id = rule_id;
      profile.kind = static_cast<std::size_t>(rule.kind);
      profile.lhs = rule.lhs;
      profile.left = rule.left;
      profile.right = rule.right;
      if (rule_id < eq.per_rule.size()) {
        const auto &work = eq.per_rule[rule_id];
        profile.delta_rows = work.delta_rows;
        profile.delta_cells = work.delta_cells;
        profile.joins = work.joins;
        profile.propagations = work.propagations;
        profile.successful_propagations = work.successful_propagations;
        profile.repeated_outputs = work.repeated_outputs;
        profile.join_word_operations = work.join_word_operations;
      }
      result.per_rule.push_back(profile);
      auto &scc = result.per_scc[rule_scc_[rule_id]];
      scc.delta_rows += profile.delta_rows;
      scc.delta_cells += profile.delta_cells;
      scc.joins += profile.joins;
      scc.propagations += profile.propagations;
      scc.successful_propagations += profile.successful_propagations;
      scc.repeated_outputs += profile.repeated_outputs;
      scc.join_word_operations += profile.join_word_operations;
      if (profile.joins > result.hottest_rule_joins) {
        result.hottest_rule_joins = profile.joins;
        result.hottest_rule_id = rule_id;
      }
    }
    result.dependency_sccs = result.per_scc.size();
    for (const auto &scc : result.per_scc) {
      result.max_scc_symbols = std::max(result.max_scc_symbols, scc.symbols);
      result.max_scc_rules = std::max(result.max_scc_rules, scc.rules);
      if (scc.joins > result.hottest_scc_joins) {
        result.hottest_scc_joins = scc.joins;
        result.hottest_scc_id = scc.scc_id;
      }
      switch (scc.classification) {
      case EndpointQuotientSccClass::Acyclic:
        ++result.acyclic_sccs;
        break;
      case EndpointQuotientSccClass::UnaryRecursive:
        ++result.unary_recursive_sccs;
        break;
      case EndpointQuotientSccClass::LeftLinear:
      case EndpointQuotientSccClass::RightLinear:
        ++result.linear_sccs;
        break;
      case EndpointQuotientSccClass::Transitive:
        ++result.transitive_sccs;
        break;
      case EndpointQuotientSccClass::General:
        ++result.general_sccs;
        break;
      }
    }
    return result;
  }

  const Grammar &grammar_;
  endpoint::Options options_;
  endpoint::Problem base_;
  std::unique_ptr<endpoint::Solver> snapshot_;
  bool dirty_ = true;
  std::vector<SymbolId> count_symbols_;
  std::vector<Id> symbol_scc_;
  std::vector<Id> rule_scc_;
  std::vector<EndpointQuotientSccStatistics> scc_metadata_;
  std::size_t count_symbol_edges_ = 0;
  std::size_t node_count_ = 0;
  MonotoneEdgeStore edges_;
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
