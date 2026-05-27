#include "Verification/PathProgram/CompressedCfg.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/CFG.h"

#include <algorithm>
#include <functional>
#include <map>
#include <set>
#include <stack>
#include <tuple>
#include <unordered_set>

namespace lotus::verification::pathprogram {

namespace {

using Block = const llvm::BasicBlock *;

std::vector<Block> getSuccessors(Block block) {
  std::vector<Block> successors;
  for (const llvm::BasicBlock *succ : llvm::successors(block)) {
    successors.push_back(succ);
  }
  return successors;
}

} // namespace

std::size_t CfgEdgeHash::operator()(const CfgEdge &edge) const {
  const auto sourceHash =
      std::hash<const llvm::BasicBlock *>()(edge.source);
  const auto targetHash =
      std::hash<const llvm::BasicBlock *>()(edge.target);
  return sourceHash ^ (targetHash << 1);
}

CompressedCfg CompressedCfg::build(const llvm::Function &function) {
  CompressedCfg cfg;
  cfg.function_ = &function;

  llvm::DenseMap<Block, unsigned> blockOrder;
  unsigned nextOrder = 0;
  for (const llvm::BasicBlock &block : function) {
    blockOrder[&block] = nextOrder++;
  }

  unsigned nextIndex = 0;
  llvm::DenseMap<Block, unsigned> indices;
  llvm::DenseMap<Block, unsigned> lowlinks;
  std::unordered_set<Block> onStack;
  std::vector<Block> stack;
  std::vector<std::vector<Block>> components;

  std::function<void(Block)> strongConnect = [&](Block block) {
    indices[block] = nextIndex;
    lowlinks[block] = nextIndex;
    ++nextIndex;
    stack.push_back(block);
    onStack.insert(block);

    for (Block successor : getSuccessors(block)) {
      if (indices.find(successor) == indices.end()) {
        strongConnect(successor);
        lowlinks[block] = std::min(lowlinks[block], lowlinks[successor]);
      } else if (onStack.count(successor) != 0) {
        lowlinks[block] = std::min(lowlinks[block], indices[successor]);
      }
    }

    if (lowlinks[block] != indices[block]) {
      return;
    }

    std::vector<Block> component;
    while (!stack.empty()) {
      Block top = stack.back();
      stack.pop_back();
      onStack.erase(top);
      component.push_back(top);
      if (top == block) {
        break;
      }
    }

    llvm::sort(component, [&](Block lhs, Block rhs) {
      return blockOrder.lookup(lhs) < blockOrder.lookup(rhs);
    });
    components.push_back(std::move(component));
  };

  for (const llvm::BasicBlock &block : function) {
    if (indices.find(&block) == indices.end()) {
      strongConnect(&block);
    }
  }

  llvm::sort(components, [&](const auto &lhs, const auto &rhs) {
    return blockOrder.lookup(lhs.front()) < blockOrder.lookup(rhs.front());
  });

  cfg.nodes_.reserve(components.size());
  cfg.successorEdges_.resize(components.size());
  cfg.predecessorEdges_.resize(components.size());

  for (unsigned nodeId = 0; nodeId < components.size(); ++nodeId) {
    cfg.nodes_.push_back({nodeId, components[nodeId], {}});
    for (Block block : components[nodeId]) {
      cfg.blockToNode_[block] = nodeId;
    }
  }

  cfg.entryNode_ = cfg.blockToNode_.lookup(&function.getEntryBlock());

  using EdgeKey = std::pair<NodeId, NodeId>;
  std::map<EdgeKey, std::vector<CfgEdge>> compressedEdges;
  std::set<NodeId> exitNodes;

  for (const llvm::BasicBlock &block : function) {
    const NodeId sourceNode = cfg.blockToNode_.lookup(&block);
    auto successors = getSuccessors(&block);
    if (successors.empty()) {
      exitNodes.insert(sourceNode);
    }

    for (Block successor : successors) {
      const NodeId targetNode = cfg.blockToNode_.lookup(successor);
      CfgEdge originalEdge{&block, successor};
      if (sourceNode == targetNode) {
        cfg.nodes_[sourceNode].internalEdges.push_back(originalEdge);
        continue;
      }

      compressedEdges[{sourceNode, targetNode}].push_back(originalEdge);
    }
  }

  cfg.exitNodes_.assign(exitNodes.begin(), exitNodes.end());

  for (const auto &[key, originalEdges] : compressedEdges) {
    const EdgeId edgeId = cfg.edges_.size();
    cfg.edges_.push_back({edgeId, key.first, key.second, originalEdges});
    cfg.successorEdges_[key.first].push_back(edgeId);
    cfg.predecessorEdges_[key.second].push_back(edgeId);
  }

  return cfg;
}

const CompressedCfg::Node &CompressedCfg::getNode(NodeId id) const {
  return nodes_.at(id);
}

const CompressedCfg::Edge &CompressedCfg::getEdge(EdgeId id) const {
  return edges_.at(id);
}

std::optional<CompressedCfg::NodeId>
CompressedCfg::nodeContaining(const llvm::BasicBlock &block) const {
  auto it = blockToNode_.find(&block);
  if (it == blockToNode_.end()) {
    return std::nullopt;
  }
  return it->second;
}

std::optional<CompressedCfg::EdgeId>
CompressedCfg::edgeBetween(NodeId source, NodeId target) const {
  for (EdgeId edgeId : successorEdges(source)) {
    const Edge &edge = getEdge(edgeId);
    if (edge.target == target) {
      return edgeId;
    }
  }
  return std::nullopt;
}

llvm::ArrayRef<CompressedCfg::EdgeId>
CompressedCfg::successorEdges(NodeId node) const {
  return successorEdges_.at(node);
}

llvm::ArrayRef<CompressedCfg::EdgeId>
CompressedCfg::predecessorEdges(NodeId node) const {
  return predecessorEdges_.at(node);
}

} // namespace lotus::verification::pathprogram
