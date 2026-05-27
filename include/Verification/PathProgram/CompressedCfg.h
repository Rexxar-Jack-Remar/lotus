#pragma once

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Function.h"

#include <optional>
#include <utility>
#include <vector>

namespace lotus::verification::pathprogram {

struct CfgEdge {
  const llvm::BasicBlock *source = nullptr;
  const llvm::BasicBlock *target = nullptr;

  bool operator==(const CfgEdge &other) const {
    return source == other.source && target == other.target;
  }
};

struct CfgEdgeHash {
  std::size_t operator()(const CfgEdge &edge) const;
};

/// Utility for SCC-compressing a function CFG.
///
/// This is intentionally separate from the PLDI'07 path-program abstraction.
/// A path program is represented by PathProgram/PathProgramBuilder; CompressedCfg
/// is supporting infrastructure for later SCC/MSCC-based path enumeration work.
class CompressedCfg {
public:
  using NodeId = unsigned;
  using EdgeId = unsigned;

  struct Node {
    NodeId id = 0;
    std::vector<const llvm::BasicBlock *> blocks;
    std::vector<CfgEdge> internalEdges;
  };

  struct Edge {
    EdgeId id = 0;
    NodeId source = 0;
    NodeId target = 0;
    std::vector<CfgEdge> originalEdges;
  };

  static CompressedCfg build(const llvm::Function &function);

  const llvm::Function &function() const { return *function_; }
  llvm::ArrayRef<Node> nodes() const { return nodes_; }
  llvm::ArrayRef<Edge> edges() const { return edges_; }
  llvm::ArrayRef<NodeId> exitNodes() const { return exitNodes_; }

  const Node &getNode(NodeId id) const;
  const Edge &getEdge(EdgeId id) const;

  NodeId entryNode() const { return entryNode_; }

  std::optional<NodeId>
  nodeContaining(const llvm::BasicBlock &block) const;

  std::optional<EdgeId> edgeBetween(NodeId source, NodeId target) const;

  llvm::ArrayRef<EdgeId> successorEdges(NodeId node) const;
  llvm::ArrayRef<EdgeId> predecessorEdges(NodeId node) const;

private:
  const llvm::Function *function_ = nullptr;
  NodeId entryNode_ = 0;
  std::vector<Node> nodes_;
  std::vector<Edge> edges_;
  std::vector<NodeId> exitNodes_;
  std::vector<std::vector<EdgeId>> successorEdges_;
  std::vector<std::vector<EdgeId>> predecessorEdges_;
  llvm::DenseMap<const llvm::BasicBlock *, NodeId> blockToNode_;
};

} // namespace lotus::verification::pathprogram
