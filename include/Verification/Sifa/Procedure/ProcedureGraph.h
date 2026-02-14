//===-- Verification/Sifa/Procedure/ProcedureGraph.h ----------------------===//
//
// Intraprocedural CFG wrapper as a labeled graph suitable for path expressions.
//
// Nodes: llvm::BasicBlock* (plus a nullptr EXIT sink used as a sentinel).
// Labels: Transition (CFG edge, marker, or synthetic call/return summary edge)
//
//===----------------------------------------------------------------------===//

#ifndef LOTUS_VERIFICATION_SIFA_PROCEDURE_PROCEDUREGRAPH_H
#define LOTUS_VERIFICATION_SIFA_PROCEDURE_PROCEDUREGRAPH_H

#include "Utils/Algorithms/PathExpressions/LabeledGraph.h"
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

/// Side-table information for a Transition id.
///
/// The ProcedureGraph assigns a dense id to every labeled edge it creates.
/// That id is stored in Transition::id and indexes into this vector, allowing
/// other components (e.g. debug/logging/analysis) to recover the underlying
/// CFG endpoints and (for ReturnSummary edges) the callee.
struct TransitionInfo {
  llvm::BasicBlock *source = nullptr;
  llvm::BasicBlock *target = nullptr;
  llvm::Function *callee = nullptr; // non-null for ReturnSummary edges
};

/// Intraprocedural procedure graph used as input to the path-expression engine.
///
/// This is a thin wrapper around an LLVM function's CFG that:
/// - assigns stable, dense edge ids (used as the alphabet for regex/DAG),
/// - represents all "returns" using one synthetic EXIT sink node (`nullptr`),
/// - optionally includes synthetic call/return summary edges used by
///   interprocedural interpretation.
///
/// Notes on the EXIT sentinel:
/// - The graph's node type is `llvm::BasicBlock*`; we additionally allow
///   `nullptr` to appear as a *target* to represent procedure exit.
/// - `nullptr` is *not* added as a node via addNode(); it is only used as an
///   edge target and as `exitNode_`.
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
  /// EXIT sentinel. For graphs built from an LLVM function this is always null,
  /// and procedure exits are represented by edges `bb -> nullptr`.
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
