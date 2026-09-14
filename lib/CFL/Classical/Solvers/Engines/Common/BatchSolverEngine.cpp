#include "CFL/Classical/Solvers/Engines/Common/BatchSolverEngine.h"

#include "CFL/Classical/Solvers/Engines/CAT/ContextAwareTabulation.h"
#include "CFL/Classical/Solvers/Engines/IEOCE/IterativeEpoch.h"

#include <limits>
#include <numeric>
#include <optional>
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
  common::Grammar grammar;
  std::vector<common::Symbol> seed_symbols;
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
  const std::size_t max_symbols = std::numeric_limits<common::Symbol>::max();
  if (grammar.symbolCount() > max_symbols ||
      nonterminals > max_symbols - grammar.symbolCount()) {
    throw common::ResourceLimit("too many symbols for modern CFL import");
  }

  for (const auto &[head, rules] : grammar.productions()) {
    const SymbolId lhs = grammar.symbolId(head);
    for (const auto &rule : rules) {
      if (rule.size() == 1 && rule.front() == Grammar::kEpsilonSymbol) {
        result.grammar.rules.push_back(common::Rule::epsilon(lhs));
      } else if (rule.size() == 1) {
        result.grammar.rules.push_back(
            common::Rule::unary(lhs, grammar.symbolId(rule.front())));
      } else if (rule.size() == 2) {
        result.grammar.rules.push_back(common::Rule::binary(
            lhs, grammar.symbolId(rule[0]), grammar.symbolId(rule[1])));
      } else {
        throw std::logic_error(
            "CAT/IEOCE engines require a normalized grammar");
      }
    }
  }

  // SolverSession may migrate nonterminal facts after a monotone grammar
  // extension. Private terminals turn those facts into ordinary axioms while
  // preserving the modern engines' terminal-only graph boundary.
  for (SymbolId symbol = 0; symbol < grammar.symbolCount(); ++symbol) {
    if (result.grammar.terminal[symbol]) {
      result.seed_symbols[symbol] = symbol;
      continue;
    }
    const auto seed =
        static_cast<common::Symbol>(result.grammar.terminal.size());
    result.grammar.terminal.push_back(true);
    result.grammar.rules.push_back(common::Rule::unary(symbol, seed));
    result.seed_symbols[symbol] = seed;
  }
  result.grammar.validate();
  return result;
}

common::Graph makeGraph(std::size_t node_count) {
  static_assert(sizeof(NodeId) <= sizeof(common::Node),
                "Lotus node IDs must fit modern CFL node IDs");
  common::Graph graph;
  graph.nodes.resize(node_count);
  std::iota(graph.nodes.begin(), graph.nodes.end(), common::Node{0});
  return graph;
}

std::size_t checkedAdd(std::size_t lhs, std::size_t rhs) {
  if (rhs > std::numeric_limits<std::size_t>::max() - lhs)
    throw common::ResourceLimit("expanded result size overflow");
  return lhs + rhs;
}

std::size_t originalExpandedSize(const common::Reachability &reachability,
                                 std::size_t symbol_count) {
  std::size_t result = 0;
  for (SymbolId symbol = 0; symbol < symbol_count; ++symbol)
    result = checkedAdd(result, reachability.expandedSize(symbol));
  return result;
}

std::size_t originalStoredSize(const common::Reachability &reachability,
                               std::size_t symbol_count) {
  std::size_t result = 0;
  for (const common::Fact &fact : reachability.quotientFacts())
    result += fact.symbol < symbol_count ? 1 : 0;
  for (const common::Fact &fact : reachability.originalTerminalFacts())
    result += fact.symbol < symbol_count ? 1 : 0;
  return result;
}

BatchSolverStatistics collect(const cat::Result &result,
                              std::size_t symbol_count) {
  BatchSolverStatistics stats;
  stats.nodes = result.stats.nodes;
  stats.unique_base_edges = result.stats.unique_base_edges;
  stats.expanded_facts =
      originalExpandedSize(result.reachability, symbol_count);
  stats.stored_facts = originalStoredSize(result.reachability, symbol_count);
  stats.attempts = result.stats.attempts;
  stats.successful_insertions = result.stats.successful_insertions;
  stats.duplicate_attempts = result.stats.duplicate_attempts;
  stats.work_items = result.stats.work_items;
  stats.peak_worklist = result.stats.peak_worklist;
  stats.unary_applications = result.stats.unary_applications;
  stats.binary_join_pairs = result.stats.binary_join_pairs;
  stats.cat_graph_degree = result.stats.graph_degree;
  stats.cat_fully_pruned_attempts = result.stats.fully_pruned_attempts;
  stats.cat_incoming_only_insertions = result.stats.incoming_only_insertions;
  stats.cat_outgoing_only_insertions = result.stats.outgoing_only_insertions;
  stats.cat_fully_indexed_insertions = result.stats.fully_indexed_insertions;
  stats.cat_unindexed_insertions = result.stats.unindexed_insertions;
  stats.cat_propagating_insertions = result.stats.propagating_insertions;
  stats.cat_dynamic_insertions = result.stats.dynamic_insertions;
  stats.cat_promotions = result.stats.promotions;
  stats.cat_context_annotations = result.stats.context_annotations;
  stats.cat_universal_contexts = result.stats.universal_contexts;
  stats.cat_rewrites = result.transformation.events.size() +
                       result.static_skewing_rewrites.size();
  return stats;
}

BatchSolverStatistics collect(const ieoce::Result &result,
                              std::size_t symbol_count) {
  BatchSolverStatistics stats;
  stats.nodes = result.stats.nodes;
  stats.unique_base_edges = result.stats.unique_base_edges;
  stats.expanded_facts =
      originalExpandedSize(result.reachability, symbol_count);
  stats.stored_facts = originalStoredSize(result.reachability, symbol_count);
  stats.attempts = result.stats.attempts;
  stats.successful_insertions = result.stats.successful_insertions;
  stats.duplicate_attempts = result.stats.duplicate_attempts;
  stats.work_items = result.stats.work_items;
  stats.peak_worklist = result.stats.peak_worklist;
  stats.unary_applications = result.stats.unary_applications;
  stats.binary_join_pairs = result.stats.binary_join_pairs;
  stats.ieoce_quotient_nodes = result.stats.quotient_nodes;
  stats.ieoce_epochs = result.stats.epochs;
  stats.ieoce_scc_passes = result.stats.scc_passes;
  stats.ieoce_collapsed_components = result.stats.collapsed_components;
  stats.ieoce_merged_nodes = result.stats.merged_nodes;
  stats.ieoce_quotient_replays = result.stats.quotient_replays;
  stats.ieoce_graph_facts = result.stats.graph_facts;
  stats.ieoce_meg_insertions = result.stats.meg_insertions;
  stats.ieoce_meg_edges = result.stats.meg_edges;
  stats.ieoce_meg_edges_removed = result.stats.meg_edges_removed;
  stats.ieoce_transitive_updates = result.stats.transitive_updates;
  stats.ieoce_ordered_steps = result.stats.ordered_steps;
  stats.ieoce_ordered_prunes = result.stats.ordered_prunes;
  stats.ieoce_ordinary_fallback = result.stats.ordinary_fallback;
  stats.ieoce_ordered_enabled = result.stats.ordered_enabled;
  return stats;
}

void clearOperationCounters(BatchSolverStatistics &stats) {
  stats.derived_facts = 0;
  stats.attempts = 0;
  stats.successful_insertions = 0;
  stats.duplicate_attempts = 0;
  stats.work_items = 0;
  stats.peak_worklist = 0;
  stats.unary_applications = 0;
  stats.binary_join_pairs = 0;
  stats.cat_fully_pruned_attempts = 0;
  stats.cat_incoming_only_insertions = 0;
  stats.cat_outgoing_only_insertions = 0;
  stats.cat_fully_indexed_insertions = 0;
  stats.cat_unindexed_insertions = 0;
  stats.cat_propagating_insertions = 0;
  stats.cat_dynamic_insertions = 0;
  stats.cat_promotions = 0;
  stats.cat_context_annotations = 0;
  stats.cat_universal_contexts = 0;
  stats.ieoce_epochs = 0;
  stats.ieoce_scc_passes = 0;
  stats.ieoce_collapsed_components = 0;
  stats.ieoce_merged_nodes = 0;
  stats.ieoce_quotient_replays = 0;
  stats.ieoce_meg_insertions = 0;
  stats.ieoce_meg_edges_removed = 0;
  stats.ieoce_transitive_updates = 0;
  stats.ieoce_ordered_steps = 0;
  stats.ieoce_ordered_prunes = 0;
}

} // namespace

class BatchSolverEngine::Impl {
public:
  Impl(const Grammar &grammar, std::size_t node_count, BatchEngineKind variant)
      : variant_(variant), imported_(importGrammar(grammar)),
        graph_(makeGraph(node_count)), node_count_(node_count) {}

  void ensureNodeCount(std::size_t node_count) {
    if (node_count <= node_count_)
      return;
    const std::size_t old_size = graph_.nodes.size();
    try {
      for (NodeId node = node_count_; node < node_count; ++node)
        graph_.nodes.push_back(node);
    } catch (...) {
      graph_.nodes.resize(old_size);
      throw;
    }
    node_count_ = node_count;
    dirty_ = true;
  }

  bool add(SymbolId symbol, NodeId source, NodeId target) {
    if (symbol >= imported_.original_symbols || source >= node_count_ ||
        target >= node_count_)
      throw std::out_of_range("CAT/IEOCE input ID out of range");
    const EdgeKey key{symbol, source, target};
    const auto known = known_edges_.insert(key);
    if (!known.second)
      return false;
    try {
      pending_inputs_.insert(key);
      graph_.edges.push_back({source, imported_.seed_symbols[symbol], target});
    } catch (...) {
      pending_inputs_.erase(key);
      known_edges_.erase(known.first);
      throw;
    }
    dirty_ = true;
    return true;
  }

  BatchSolverStatistics solve() {
    if (!dirty_) {
      clearOperationCounters(stats_);
      return stats_;
    }

    common::Reachability next;
    BatchSolverStatistics next_stats;
    if (variant_ == BatchEngineKind::Cat) {
      cat::Options options;
      options.query.scope = common::Scope::AllSymbols;
      cat::Result result = cat::solve(imported_.grammar, graph_, options);
      next_stats = collect(result, imported_.original_symbols);
      next = std::move(result.reachability);
    } else {
      ieoce::Options options;
      options.query.scope = common::Scope::AllSymbols;
      options.variant = variant_ == BatchEngineKind::Iea
                            ? ieoce::Variant::Iea
                            : ieoce::Variant::IeaOcr;
      ieoce::Result result = ieoce::solve(imported_.grammar, graph_, options);
      next_stats = collect(result, imported_.original_symbols);
      next = std::move(result.reachability);
    }

    const std::size_t previous_facts =
        snapshot_ ? originalExpandedSize(*snapshot_, imported_.original_symbols)
                  : 0;
    std::size_t newly_visible_inputs = 0;
    for (const EdgeKey &edge : pending_inputs_)
      if (!snapshot_ ||
          !snapshot_->contains(edge.source, edge.symbol, edge.target))
        ++newly_visible_inputs;
    if (next_stats.expanded_facts < previous_facts + newly_visible_inputs)
      throw std::logic_error("CAT/IEOCE rebuild was not monotone");
    next_stats.derived_facts =
        next_stats.expanded_facts - previous_facts - newly_visible_inputs;

    snapshot_ = std::move(next);
    pending_inputs_.clear();
    dirty_ = false;
    stats_ = next_stats;
    return stats_;
  }

  bool contains(SymbolId symbol, NodeId source, NodeId target) const {
    return snapshot_ && symbol < imported_.original_symbols &&
           source < node_count_ && target < node_count_ &&
           snapshot_->contains(source, symbol, target);
  }

  bool visitSuccessors(SymbolId symbol, NodeId source,
                       NodeVisitor visitor) const {
    if (source >= node_count_)
      throw std::out_of_range("CAT/IEOCE source out of range");
    return !snapshot_ || symbol >= imported_.original_symbols ||
           snapshot_->visitSuccessors(source, symbol, visitor);
  }

  bool visitPredecessors(SymbolId symbol, NodeId target,
                         NodeVisitor visitor) const {
    if (target >= node_count_)
      throw std::out_of_range("CAT/IEOCE target out of range");
    return !snapshot_ || symbol >= imported_.original_symbols ||
           snapshot_->visitPredecessors(symbol, target, visitor);
  }

  bool visitEdges(SymbolId symbol, EdgeVisitor visitor) const {
    if (!snapshot_ || symbol >= imported_.original_symbols)
      return true;
    return snapshot_->visitFacts(symbol, [&](const common::Fact &fact) {
      return visitor({fact.symbol, static_cast<NodeId>(fact.source),
                      static_cast<NodeId>(fact.target)});
    });
  }

  bool visitEdges(EdgeVisitor visitor) const {
    for (SymbolId symbol = 0; symbol < imported_.original_symbols; ++symbol)
      if (!visitEdges(symbol, visitor))
        return false;
    return true;
  }

  std::size_t edgeCount() const {
    return snapshot_
               ? originalExpandedSize(*snapshot_, imported_.original_symbols)
               : 0;
  }

  std::size_t edgeCount(SymbolId symbol) const {
    return snapshot_ && symbol < imported_.original_symbols
               ? snapshot_->expandedSize(symbol)
               : 0;
  }

  std::size_t estimatedPayloadBytes() const {
    std::size_t bytes =
        sizeof(Impl) + graph_.nodes.capacity() * sizeof(common::Node) +
        graph_.edges.capacity() * sizeof(common::Fact) +
        imported_.grammar.terminal.capacity() * sizeof(bool) +
        imported_.grammar.rules.capacity() * sizeof(common::Rule) +
        imported_.seed_symbols.capacity() * sizeof(common::Symbol) +
        known_edges_.size() * sizeof(EdgeKey) +
        known_edges_.bucket_count() * sizeof(void *) +
        pending_inputs_.size() * sizeof(EdgeKey) +
        pending_inputs_.bucket_count() * sizeof(void *);
    if (snapshot_)
      bytes += snapshot_->nodes().size() * sizeof(common::Node) +
               snapshot_->representatives().size() * sizeof(common::Node) +
               snapshot_->outputSymbols().size() * sizeof(common::Symbol) +
               snapshot_->quotientFacts().size() * sizeof(common::Fact) +
               snapshot_->originalTerminalFacts().size() * sizeof(common::Fact);
    return bytes;
  }

  const BatchSolverStatistics &statistics() const { return stats_; }

private:
  BatchEngineKind variant_;
  ImportedGrammar imported_;
  common::Graph graph_;
  std::optional<common::Reachability> snapshot_;
  std::unordered_set<EdgeKey, EdgeKeyHash> known_edges_;
  std::unordered_set<EdgeKey, EdgeKeyHash> pending_inputs_;
  std::size_t node_count_ = 0;
  bool dirty_ = true;
  BatchSolverStatistics stats_;
};

BatchSolverEngine::BatchSolverEngine(const Grammar &grammar,
                                     std::size_t node_count,
                                     BatchEngineKind variant)
    : impl_(std::make_unique<Impl>(grammar, node_count, variant)) {}

BatchSolverEngine::~BatchSolverEngine() = default;

void BatchSolverEngine::ensureNodeCount(std::size_t node_count) {
  impl_->ensureNodeCount(node_count);
}

bool BatchSolverEngine::add(SymbolId symbol, NodeId source, NodeId target) {
  return impl_->add(symbol, source, target);
}

BatchSolverStatistics BatchSolverEngine::solve() { return impl_->solve(); }

const BatchSolverStatistics &BatchSolverEngine::statistics() const {
  return impl_->statistics();
}

bool BatchSolverEngine::contains(SymbolId symbol, NodeId source,
                                 NodeId target) const {
  return impl_->contains(symbol, source, target);
}

bool BatchSolverEngine::visitSuccessors(SymbolId symbol, NodeId source,
                                        NodeVisitor visitor) const {
  return impl_->visitSuccessors(symbol, source, visitor);
}

bool BatchSolverEngine::visitPredecessors(SymbolId symbol, NodeId target,
                                          NodeVisitor visitor) const {
  return impl_->visitPredecessors(symbol, target, visitor);
}

bool BatchSolverEngine::visitEdges(EdgeVisitor visitor) const {
  return impl_->visitEdges(visitor);
}

bool BatchSolverEngine::visitEdges(SymbolId symbol, EdgeVisitor visitor) const {
  return impl_->visitEdges(symbol, visitor);
}

std::size_t BatchSolverEngine::edgeCount() const { return impl_->edgeCount(); }

std::size_t BatchSolverEngine::edgeCount(SymbolId symbol) const {
  return impl_->edgeCount(symbol);
}

std::size_t BatchSolverEngine::estimatedPayloadBytes() const {
  return impl_->estimatedPayloadBytes();
}

} // namespace lotus::cfl::classical::engines
