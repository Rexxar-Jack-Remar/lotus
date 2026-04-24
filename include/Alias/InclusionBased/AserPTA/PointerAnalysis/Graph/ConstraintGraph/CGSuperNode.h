//
// Created by peiming on 11/2/19.
//
#ifndef ASER_PTA_CGSUPERNODE_H
#define ASER_PTA_CGSUPERNODE_H

// TODO: maybe merge SCC later?

#include "Alias/InclusionBased/AserPTA/PointerAnalysis/Graph/ConstraintGraph/CGObjNode.h"
#include "Alias/InclusionBased/AserPTA/PointerAnalysis/Graph/ConstraintGraph/CGPtrNode.h"

namespace aser {

/// @brief Constraint graph node representing collapsed strongly connected
/// components (SCCs).
///
/// CGSuperNode merges multiple nodes in a cycle into a single representative
/// node to improve constraint solving efficiency. All constraints pointing to
/// or from nodes in the SCC are redirected to the super node.
///
/// @tparam ctx Context type for context-sensitive analysis
template <typename ctx> class CGSuperNode : public CGNodeBase<ctx> {
private:
  using super = CGNodeBase<ctx>;
  using NodeList = llvm::SparseBitVector<>;

  NodeList scc;
  NodeList callNodes;

  CGSuperNode(const std::vector<super *> &scc, NodeID id)
      : super(id, CGNodeKind::SuperNode) {
    for (super *node : scc) {
      if (node->isFunctionPtr()) {
        callNodes.set(node->getNodeID());
      }
      if (auto superNode = llvm::dyn_cast<CGSuperNode<ctx>>(node)) {
        // merge a super node
        this->scc |= superNode->scc;
        this->callNodes |= superNode->callNodes;
      }
      this->scc.set(node->getNodeID());
    }
  };

public:
  /// @brief Copies all outgoing edges from a node to this super node.
  /// @param node The node whose outgoing edges should be copied
  inline void copyOutgoingEdges(super *node) {
    for (auto it = node->pred_edge_begin(), ie = node->pred_edge_end();
         it != ie; it++) {
      auto pred = (*it).second;
      auto edgeKind = (*it).first;
      // FIXME: is this correct?
      pred->insertConstraint(this, edgeKind);
    }

    node->setSuperNode(this->getNodeID());
  }

  /// @brief Merges all incoming edges from SCC nodes and clears individual node
  /// constraints.
  ///
  /// Consolidates constraints from all nodes in the SCC into this super node,
  /// then clears the original constraints to prevent redundant processing.
  void clearAndMergeIncomingEdges() {
    for (NodeID nodeId : scc) {
      auto node = this->graph->getNode(nodeId);
      // merge the incoming edges
      for (auto it = node->edge_begin(), ie = node->edge_end(); it != ie;
           it++) {
        this->insertConstraint((*it).second, (*it).first);
      }
    }

    for (NodeID nodeId : scc) {
      auto node = this->graph->getNode(nodeId);
      node->clearConstraints();
    }

    this->removeConstraint(this, Constraints::copy);
  }

  static inline bool classof(const super *node) {
    return node->getType() == CGNodeKind::SuperNode;
  }

  virtual std::string toString() const { return "SuperNode"; }

  friend class GraphBase<super, Constraints>;
  friend class ConstraintGraph<ctx>;
};

} // namespace aser

#endif