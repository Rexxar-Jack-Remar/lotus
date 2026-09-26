#include "CFL/Classical/Core/Relation.h"

#include <algorithm>
#include <limits>
#include <map>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include <llvm/ADT/SparseBitVector.h>

namespace lotus::cfl::classical {
namespace {

class SparseSetRelation final : public Relation {
public:
  explicit SparseSetRelation(std::size_t node_count)
      : successors_(node_count), predecessors_(node_count) {}

  void ensureNodeCount(std::size_t node_count) override {
    successors_.resize(node_count);
    predecessors_.resize(node_count);
  }

  bool add(SymbolId symbol, NodeId source, NodeId target) override {
    auto &targets = successors_.at(source)[symbol];
    if (!targets.insert(target).second) {
      return false;
    }
    predecessors_.at(target)[symbol].insert(source);
    ++edge_count_;
    ++symbol_edge_counts_[symbol];
    return true;
  }

  bool contains(SymbolId symbol, NodeId source, NodeId target) const override {
    const auto &by_symbol = successors_.at(source);
    const auto it = by_symbol.find(symbol);
    return it != by_symbol.end() && it->second.count(target) != 0;
  }

  bool visitSuccessors(SymbolId symbol, NodeId source,
                       NodeVisitor visitor) const override {
    return visit(successors_.at(source), symbol, visitor);
  }

  bool visitPredecessors(SymbolId symbol, NodeId target,
                         NodeVisitor visitor) const override {
    return visit(predecessors_.at(target), symbol, visitor);
  }

  bool visitEdges(EdgeVisitor visitor) const override {
    for (NodeId source = 0; source < successors_.size(); ++source)
      for (const auto &[symbol, targets] : successors_[source])
        for (NodeId target : targets)
          if (!visitor({symbol, source, target}))
            return false;
    return true;
  }

  bool visitEdges(SymbolId symbol, EdgeVisitor visitor) const override {
    for (NodeId source = 0; source < successors_.size(); ++source) {
      const auto it = successors_[source].find(symbol);
      if (it != successors_[source].end())
        for (NodeId target : it->second)
          if (!visitor({symbol, source, target}))
            return false;
    }
    return true;
  }

  std::size_t edgeCount() const override { return edge_count_; }

  std::size_t edgeCount(SymbolId symbol) const override {
    const auto it = symbol_edge_counts_.find(symbol);
    return it == symbol_edge_counts_.end() ? 0 : it->second;
  }

  std::size_t estimatedPayloadBytes() const override {
    std::size_t bytes = sizeof(*this);
    bytes +=
        (successors_.capacity() + predecessors_.capacity()) * sizeof(SymbolMap);
    for (const auto &nodes : {&successors_, &predecessors_}) {
      for (const SymbolMap &map : *nodes) {
        bytes += map.size() * sizeof(SymbolMap::value_type);
        for (const auto &[_, values] : map) {
          bytes += values.size() * sizeof(NodeId);
        }
      }
    }
    return bytes;
  }

private:
  using NodeSet = std::unordered_set<NodeId>;
  using SymbolMap = std::unordered_map<SymbolId, NodeSet>;

  static bool visit(const SymbolMap &map, SymbolId symbol,
                    NodeVisitor visitor) {
    const auto it = map.find(symbol);
    if (it != map.end())
      for (NodeId node : it->second)
        if (!visitor(node))
          return false;
    return true;
  }

  std::vector<SymbolMap> successors_;
  std::vector<SymbolMap> predecessors_;
  std::size_t edge_count_ = 0;
  std::unordered_map<SymbolId, std::size_t> symbol_edge_counts_;
};

class SparseBitVectorRelation final : public Relation {
public:
  explicit SparseBitVectorRelation(std::size_t node_count) {
    ensureNodeCount(node_count);
  }

  void ensureNodeCount(std::size_t node_count) override {
    if (node_count > std::numeric_limits<unsigned>::max()) {
      throw std::overflow_error(
          "SparseBitVector relation node count exceeds unsigned range");
    }
    successors_.resize(node_count);
    predecessors_.resize(node_count);
  }

  bool add(SymbolId symbol, NodeId source, NodeId target) override {
    auto &targets = getOrCreate(successors_.at(source), symbol);
    if (!targets.test_and_set(target)) {
      return false;
    }
    getOrCreate(predecessors_.at(target), symbol).set(source);
    ++edge_count_;
    ++symbol_edge_counts_[symbol];
    return true;
  }

  bool contains(SymbolId symbol, NodeId source, NodeId target) const override {
    const auto *targets = find(successors_.at(source), symbol);
    return targets != nullptr && targets->test(target);
  }

  bool visitSuccessors(SymbolId symbol, NodeId source,
                       NodeVisitor visitor) const override {
    return visit(successors_.at(source), symbol, visitor);
  }

  bool visitPredecessors(SymbolId symbol, NodeId target,
                         NodeVisitor visitor) const override {
    return visit(predecessors_.at(target), symbol, visitor);
  }

  bool visitEdges(EdgeVisitor visitor) const override {
    for (NodeId source = 0; source < successors_.size(); ++source)
      for (const auto &[symbol, targets] : successors_[source])
        for (NodeId target : targets)
          if (!visitor({symbol, source, target}))
            return false;
    return true;
  }

  bool visitEdges(SymbolId symbol, EdgeVisitor visitor) const override {
    for (NodeId source = 0; source < successors_.size(); ++source) {
      const auto *targets = find(successors_[source], symbol);
      if (targets != nullptr)
        for (NodeId target : *targets)
          if (!visitor({symbol, source, target}))
            return false;
    }
    return true;
  }

  std::size_t edgeCount() const override { return edge_count_; }

  std::size_t edgeCount(SymbolId symbol) const override {
    const auto it = symbol_edge_counts_.find(symbol);
    return it == symbol_edge_counts_.end() ? 0 : it->second;
  }

  std::size_t estimatedPayloadBytes() const override {
    std::size_t bytes = sizeof(*this);
    bytes +=
        (successors_.capacity() + predecessors_.capacity()) * sizeof(SymbolMap);
    for (const auto &nodes : {&successors_, &predecessors_}) {
      for (const SymbolMap &map : *nodes) {
        bytes += map.capacity() * sizeof(std::pair<SymbolId, BitVector>);
        for (const auto &[_, values] : map) {
          bytes += values.count() * sizeof(unsigned);
        }
      }
    }
    return bytes;
  }

private:
  using BitVector = llvm::SparseBitVector<>;
  using SymbolMap = std::vector<std::pair<SymbolId, BitVector>>;

  static const BitVector *find(const SymbolMap &map, SymbolId symbol) {
    auto it = std::lower_bound(
        map.begin(), map.end(), symbol,
        [](const std::pair<SymbolId, BitVector> &entry, SymbolId s) {
          return entry.first < s;
        });
    if (it != map.end() && it->first == symbol) {
      return &it->second;
    }
    return nullptr;
  }

  static BitVector &getOrCreate(SymbolMap &map, SymbolId symbol) {
    auto it = std::lower_bound(
        map.begin(), map.end(), symbol,
        [](const std::pair<SymbolId, BitVector> &entry, SymbolId s) {
          return entry.first < s;
        });
    if (it != map.end() && it->first == symbol) {
      return it->second;
    }
    return map.emplace(it, symbol, BitVector{})->second;
  }

  static bool visit(const SymbolMap &map, SymbolId symbol,
                    NodeVisitor visitor) {
    const auto *targets = find(map, symbol);
    if (targets != nullptr) {
      for (NodeId node : *targets) {
        if (!visitor(node)) {
          return false;
        }
      }
    }
    return true;
  }

  std::vector<SymbolMap> successors_;
  std::vector<SymbolMap> predecessors_;
  std::size_t edge_count_ = 0;
  std::unordered_map<SymbolId, std::size_t> symbol_edge_counts_;
};

} // namespace

void Relation::forEachSuccessor(
    SymbolId symbol, NodeId source,
    llvm::function_ref<void(NodeId)> visitor) const {
  visitSuccessors(symbol, source, [&](NodeId target) {
    visitor(target);
    return true;
  });
}

void Relation::forEachPredecessor(
    SymbolId symbol, NodeId target,
    llvm::function_ref<void(NodeId)> visitor) const {
  visitPredecessors(symbol, target, [&](NodeId source) {
    visitor(source);
    return true;
  });
}

std::vector<RelationEdge> Relation::edges() const {
  std::vector<RelationEdge> result;
  result.reserve(edgeCount());
  visitEdges([&](const RelationEdge &edge) {
    result.push_back(edge);
    return true;
  });
  return result;
}

std::vector<RelationEdge> Relation::edges(SymbolId symbol) const {
  std::vector<RelationEdge> result;
  result.reserve(edgeCount(symbol));
  visitEdges(symbol, [&](const RelationEdge &edge) {
    result.push_back(edge);
    return true;
  });
  return result;
}

std::unique_ptr<Relation> createRelation(RelationBackend backend,
                                         std::size_t node_count) {
  if (backend == RelationBackend::SparseBitVectors) {
    return std::make_unique<SparseBitVectorRelation>(node_count);
  }
  return std::make_unique<SparseSetRelation>(node_count);
}

} // namespace lotus::cfl::classical
