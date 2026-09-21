#include "CFL/Classical/Solvers/Engines/CERT/CertCFLEngine.h"

#include <limits>
#include <stdexcept>
#include <type_traits>
#include <unordered_set>
#include <utility>

namespace lotus::cfl::classical::engines {
namespace {
static_assert(std::is_same<NodeId, cert::Node>::value, "Lotus NodeId changed");
static_assert(std::is_same<SymbolId, cert::Symbol>::value, "Lotus SymbolId changed");
std::size_t addSize(std::size_t a, std::size_t b) {
  if (b > std::numeric_limits<std::size_t>::max() - a)
    throw std::overflow_error("CERT-CFL payload estimate overflow");
  return a + b;
}
std::size_t mulSize(std::size_t a, std::size_t b) {
  if (a && b > std::numeric_limits<std::size_t>::max() / a)
    throw std::overflow_error("CERT-CFL payload estimate overflow");
  return a * b;
}
} // namespace

class CertCFLEngine::Impl {
public:
  Impl(const Grammar &grammar, std::size_t nodes, cert::Options options)
      : options_(std::move(options)) {
    if (options_.observed)
      throw std::invalid_argument(
          "CertCFLEngine must resolve all labels; use cert::solve for selected outputs");
    base_.nodes = nodes; base_.symbols = grammar.symbolCount();
    for (const auto &issue : grammar.validate())
      if (issue.severity == GrammarIssueSeverity::Error)
        throw std::invalid_argument(issue.message);
    base_.epsilon = grammar.nullableSymbolIds();
    for (const auto &entry : grammar.unaryByRhsId())
      for (SymbolId lhs : entry.second) base_.unary.push_back({lhs, entry.first});
    // Each normalized binary production occurs exactly once in this index.
    // Do not read both indices and accidentally count every rule twice.
    for (const auto &entry : grammar.binaryByFirstId())
      for (const auto &rule : entry.second)
        base_.binary.push_back({rule.lhs, rule.first, rule.second});
  }
  void requireSymbol(SymbolId a) const {
    if (a >= base_.symbols) throw std::out_of_range("CERT-CFL symbol out of range");
  }
  void requireNode(NodeId u) const {
    if (u >= base_.nodes) throw std::out_of_range("CERT-CFL node out of range");
  }
  void ensureNodeCount(std::size_t nodes) {
    if (nodes <= base_.nodes) return;
    if (!options_.initial_partition.empty()) {
      std::vector<NodeId> added;
      added.reserve(nodes - base_.nodes);
      for (NodeId u = base_.nodes; u < nodes; ++u) added.push_back(u);
      // push_back completes before publishing the larger node count.
      options_.initial_partition.push_back(std::move(added));
    }
    base_.nodes = nodes; dirty_ = true;
  }
  bool add(SymbolId a, NodeId u, NodeId v) {
    requireSymbol(a); requireNode(u); requireNode(v);
    const bool added = seeds_.insert(cert::Seed{a, u, v}).second;
    if (added) dirty_ = true;
    else ++duplicate_inputs_;
    return added;
  }
  CertCFLStatistics solve() {
    if (!dirty_) {
      // Sizes persist; work counters describe this call only.
      CertCFLStatistics now;
      now.logical_facts = stats_.logical_facts; now.seed_facts = stats_.seed_facts;
      now.core.final_blocks = stats_.core.final_blocks;
      now.core.explicit_seed_symbols = stats_.core.explicit_seed_symbols;
      now.core.used_sparse_fallback = stats_.core.used_sparse_fallback;
      now.core.sparse_facts = stats_.core.sparse_facts;
      now.duplicate_inputs = duplicate_inputs_;
      stats_ = now; duplicate_inputs_ = 0;
      return stats_;
    }
    auto problem = base_;
    problem.seeds.reserve(seeds_.size());
    for (const auto &e : seeds_) problem.seeds.push_back(e);
    auto next = std::make_unique<cert::Result>(cert::solve(problem, options_));
    CertCFLStatistics now;
    now.core = next->statistics(); now.logical_facts = next->edgeCount();
    now.seed_facts = seeds_.size(); now.duplicate_inputs = duplicate_inputs_;
    const auto previous = snapshot_ ? snapshot_->edgeCount() : 0;
    if (now.logical_facts < previous)
      throw std::logic_error("CERT-CFL monotone snapshot lost facts");
    now.added_facts = now.logical_facts - previous;
    // All throwing work is complete. Commit result and counters together.
    snapshot_.swap(next); stats_ = now; dirty_ = false; duplicate_inputs_ = 0;
    return stats_;
  }
  cert::Problem base_;
  cert::Options options_;
  std::unordered_set<cert::Seed, cert::SeedHash> seeds_;
  std::unique_ptr<cert::Result> snapshot_;
  CertCFLStatistics stats_;
  std::size_t duplicate_inputs_ = 0;
  bool dirty_ = true;
};

CertCFLEngine::CertCFLEngine(const Grammar &grammar, std::size_t nodes, cert::Options options)
    : impl_(std::make_unique<Impl>(grammar, nodes, std::move(options))) {}
CertCFLEngine::~CertCFLEngine() = default;
void CertCFLEngine::ensureNodeCount(std::size_t nodes) { impl_->ensureNodeCount(nodes); }
bool CertCFLEngine::add(SymbolId a, NodeId u, NodeId v) { return impl_->add(a, u, v); }
CertCFLStatistics CertCFLEngine::solve() { return impl_->solve(); }
const CertCFLStatistics &CertCFLEngine::statistics() const { return impl_->stats_; }
bool CertCFLEngine::contains(SymbolId a, NodeId u, NodeId v) const {
  impl_->requireSymbol(a); impl_->requireNode(u); impl_->requireNode(v);
  const auto &snapshot = impl_->snapshot_;
  return snapshot && u < snapshot->nodeCount() && v < snapshot->nodeCount() &&
         snapshot->contains(a, u, v);
}
bool CertCFLEngine::visitSuccessors(SymbolId a, NodeId u, NodeVisitor visitor) const {
  impl_->requireSymbol(a); impl_->requireNode(u);
  const auto &snapshot = impl_->snapshot_;
  return !snapshot || u >= snapshot->nodeCount() ||
         snapshot->visitSuccessors(a, u, [&](cert::Node v) { return visitor(v); });
}
bool CertCFLEngine::visitPredecessors(SymbolId a, NodeId v, NodeVisitor visitor) const {
  impl_->requireSymbol(a); impl_->requireNode(v);
  const auto &snapshot = impl_->snapshot_;
  return !snapshot || v >= snapshot->nodeCount() ||
         snapshot->visitPredecessors(a, v, [&](cert::Node u) { return visitor(u); });
}
bool CertCFLEngine::visitEdges(EdgeVisitor visitor) const {
  return !impl_->snapshot_ || impl_->snapshot_->visitEdges([&](const cert::Seed &e) {
    const RelationEdge edge{e.symbol, e.source, e.target}; return visitor(edge);
  });
}
bool CertCFLEngine::visitEdges(SymbolId a, EdgeVisitor visitor) const {
  impl_->requireSymbol(a);
  return !impl_->snapshot_ || impl_->snapshot_->visitEdges(a, [&](const cert::Seed &e) {
    const RelationEdge edge{e.symbol, e.source, e.target}; return visitor(edge);
  });
}
std::size_t CertCFLEngine::edgeCount() const { return impl_->snapshot_ ? impl_->snapshot_->edgeCount() : 0; }
std::size_t CertCFLEngine::edgeCount(SymbolId a) const {
  impl_->requireSymbol(a); return impl_->snapshot_ ? impl_->snapshot_->edgeCount(a) : 0;
}
std::size_t CertCFLEngine::countOffDiagonalUnion(std::vector<SymbolId> symbols) const {
  for (SymbolId a : symbols) impl_->requireSymbol(a);
  return impl_->snapshot_ ? impl_->snapshot_->countOffDiagonalUnion(std::move(symbols)) : 0;
}
std::size_t CertCFLEngine::estimatedPayloadBytes() const {
  std::size_t bytes = sizeof(*this) + sizeof(Impl);
  auto add = [&](std::size_t n, std::size_t size) { bytes = addSize(bytes, mulSize(n, size)); };
  add(impl_->seeds_.size(), sizeof(cert::Seed)); add(impl_->seeds_.bucket_count(), sizeof(void *));
  add(impl_->base_.unary.capacity(), sizeof(cert::UnaryRule));
  add(impl_->base_.binary.capacity(), sizeof(cert::BinaryRule));
  add(impl_->base_.epsilon.capacity(), sizeof(cert::Symbol));
  add(impl_->options_.initial_partition.capacity(), sizeof(std::vector<cert::Node>));
  for (const auto &g : impl_->options_.initial_partition) add(g.capacity(), sizeof(cert::Node));
  if (impl_->snapshot_) bytes = addSize(bytes, impl_->snapshot_->estimatedPayloadBytes());
  return bytes;
}
} // namespace lotus::cfl::classical::engines
