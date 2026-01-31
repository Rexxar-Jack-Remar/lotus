//===-- Verification/Sifa/Procedure/ProcedureGraph.h ----------------------===//
//
// Intraprocedural CFG wrapper as a labeled graph suitable for path expressions.
//
// Nodes: llvm::BasicBlock*
// Labels: Transition (CFG edge or marker)
//
//===----------------------------------------------------------------------===//

#ifndef LOTUS_VERIFICATION_SIFA_PROCEDURE_PROCEDUREGRAPH_H
#define LOTUS_VERIFICATION_SIFA_PROCEDURE_PROCEDUREGRAPH_H

#include "Utils/General/PathExpressions/LabeledGraph.h"
#include "Verification/Sifa/Cfg/Transition.h"

#include <unordered_map>
#include <utility>
#include <vector>

namespace llvm {
class BasicBlock;
class Function;
} // namespace llvm

namespace lotus {
namespace sifa {

struct TransitionInfo {
  llvm::BasicBlock *source = nullptr;
  llvm::BasicBlock *target = nullptr;
  llvm::Function *callee = nullptr; // non-null for ReturnSummary edges
};

class ProcedureGraph {
public:
  using Node = llvm::BasicBlock *;
  using Label = Transition;
  using Graph = lotus::pathexpressions::GenericLabeledGraph<Node, Label>;

  /// Build a procedure graph from an LLVM function's full CFG.
  explicit ProcedureGraph(const llvm::Function &F);

  /// Build an empty procedure graph with only entry and exit nodes.
  /// Use addNode/addEdge (or ProcedureGraphBuilder) to add the rest.
  ProcedureGraph(Node entryNode, Node exitNode);

  Node getEntryNode() const;
  Node getExitNode() const;

  void addNode(Node n);
  void addEdge(Node src, Node dst);

  /// Adds a ReturnSummary edge from \p src to \p dst for call to \p callee.
  /// Used for interprocedural Sifa (path expression can include call/return).
  void addReturnSummaryEdge(Node src, Node dst, const llvm::Function *callee);

  const Graph &graph() const;
  const std::vector<TransitionInfo> &transitions() const;

private:
  Node entryNode_ = nullptr;
  Node exitNode_ = nullptr;
  Graph graph_;
  std::vector<TransitionInfo> transitions_;
  struct NodePairHash {
    std::size_t operator()(const std::pair<Node, Node> &p) const;
  };

  std::unordered_map<std::pair<Node, Node>, std::uint32_t, NodePairHash> edgeToId_;

  Transition addTransition(Node src, Node dst);
};

} // namespace sifa
} // namespace lotus

#endif // LOTUS_VERIFICATION_SIFA_PROCEDURE_PROCEDUREGRAPH_H
