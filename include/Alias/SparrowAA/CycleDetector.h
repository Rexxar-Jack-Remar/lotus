/**
 * @file CycleDetector.h
 * @brief Generic SCC-based cycle detector for Andersen's constraint graph.
 *
 * ## Algorithm
 *
 * `CycleDetector` implements **Nuutila's improved Tarjan SCC algorithm**
 * (Nuutila & Soisalon-Soininen, 1994).  The key difference from classic
 * Tarjan is the use of an `inComponent` set instead of a "lowlink" array,
 * which avoids redundant stack operations and is slightly faster in practice.
 *
 * The algorithm assigns a DFS timestamp to each node on first visit.  When
 * the DFS unwinds, if a node's timestamp equals its minimum reachable
 * timestamp (i.e., it is the root of an SCC), all nodes on the SCC stack
 * with a higher timestamp belong to the same SCC and are collapsed.
 *
 * ## Usage
 *
 * Subclass `CycleDetector<MyGraph>` and implement the three pure-virtual
 * hooks:
 *
 * ```cpp
 * class MyCycleDetector : public CycleDetector<MyGraph> {
 *   NodeType *getRep(NodeIndex n) override { ... }
 *   void processNodeOnCycle(const NodeType *n, const NodeType *rep) override { ... }
 *   void processCycleRepNode(const NodeType *rep) override { ... }
 *   void run() override { runOnGraph(&myGraph); }
 * };
 * ```
 *
 * - `getRep` must return the current representative of a (possibly merged)
 *   node.  During solving, nodes in the same SCC are merged; `getRep`
 *   follows the union-find chain.
 * - `processNodeOnCycle` is called for each non-representative node in an
 *   SCC.  Typically merges the node into the representative.
 * - `processCycleRepNode` is called once for the representative of each SCC
 *   (including trivial SCCs of size 1).
 *
 * @tparam GraphType  The concrete graph type; must have an
 *                    `AndersGraphTraits<GraphType>` specialisation.
 */

#ifndef ANDERSEN_CYCLEDETECTOR_H
#define ANDERSEN_CYCLEDETECTOR_H

#include "Alias/SparrowAA/GraphTraits.h"

#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/DenseSet.h>

#include <deque>
#include <stack>

/**
 * @class CycleDetector
 * @brief Abstract base class for SCC detection in Andersen's constraint graph.
 *
 * Subclasses must implement `getRep`, `processNodeOnCycle`,
 * `processCycleRepNode`, and `run`.  The DFS traversal and SCC bookkeeping
 * are handled entirely by this base class.
 *
 * @tparam GraphType  The concrete graph type.
 */
template <class GraphType> class CycleDetector {
public:
  using GraphTraits = AndersGraphTraits<GraphType>;
  using NodeType = typename GraphTraits::NodeType;
  using node_iterator = typename GraphTraits::NodeIterator;
  using child_iterator = typename GraphTraits::ChildIterator;

private:
  /// Stack of nodes whose SCC membership has not yet been finalised.
  /// Nodes are pushed when first visited and popped when their SCC root is found.
  std::stack<const NodeType *> sccStack;
  /// Maps each visited node to its DFS discovery timestamp.
  /// Nodes absent from this map have not yet been visited.
  llvm::DenseMap<const NodeType *, unsigned> dfsNum;
  /// Tracks nodes that have been assigned to a completed SCC.
  /// Corresponds to the `inComponent` array in Nuutila's algorithm.
  llvm::DenseSet<const NodeType *> inComponent;
  /// Monotonically increasing DFS timestamp counter.
  unsigned timestamp;

  // visiting each node and perform some task
  void visit(NodeType *node) {
    unsigned myTimeStamp = timestamp++;
    assert(!dfsNum.count(node) && "Revisit the same node again?");
    dfsNum[node] = myTimeStamp;

    // Traverse succecessor edges
    for (auto childItr = GraphTraits::child_begin(node),
              childIte = GraphTraits::child_end(node);
         childItr != childIte; ++childItr) {
      NodeType *succRep = getRep(*childItr);
      if (!dfsNum.count(succRep))
        visit(succRep);

      if (!inComponent.count(succRep) && dfsNum[node] > dfsNum[succRep])
        dfsNum[node] = dfsNum[succRep];
    }

    // See if we have any cycle detected
    if (myTimeStamp != dfsNum[node]) {
      // If not, push the sccStack and go on
      sccStack.push(node);
      return;
    }

    // Cycle detected
    inComponent.insert(node);
    while (!sccStack.empty()) {
      const NodeType *cycleNode = sccStack.top();
      if (dfsNum[cycleNode] < myTimeStamp)
        break;

      processNodeOnCycle(cycleNode, node);
      inComponent.insert(cycleNode);
      sccStack.pop();
    }

    processCycleRepNode(node);
  }

protected:
  // Nodes may get merged during the analysis. This function returns the merge
  // target (if the node is merged into another node) or the node itself (if the
  // nodes has not been merged into another node)
  virtual NodeType *getRep(NodeIndex node) = 0;
  // Specify how to process the non-rep nodes if a cycle is found
  virtual void processNodeOnCycle(const NodeType *node,
                                  const NodeType *repNode) = 0;
  // Specify how to process the rep nodes if a cycle is found
  virtual void processCycleRepNode(const NodeType *node) = 0;

  // Running the cycle detection algorithm on a given graph G
  void runOnGraph(GraphType *graph) {
    assert(sccStack.empty() && "sccStack is not empty before cycle detection!");
    assert(dfsNum.empty() && "dfsNum is not empty before cycle detection!");
    assert(inComponent.empty() &&
           "inComponent is not empty before cycle detection!");

    for (auto itr = GraphTraits::node_begin(graph),
              ite = GraphTraits::node_end(graph);
         itr != ite; ++itr) {
      NodeType *repNode = getRep(itr->getNodeIndex());
      if (!dfsNum.count(repNode))
        visit(repNode);
    }

    assert(sccStack.empty() && "sccStack not empty after cycle detection!");
  }

  // Running the cycle detection algorithm on a given graph node. This function
  // is used when walking through the entire graph is not the desirable
  // behavior.
  void runOnNode(NodeIndex node) {
    assert(sccStack.empty() && "sccStack is not empty before cycle detection!");

    NodeType *repNode = getRep(node);
    if (!dfsNum.count(repNode))
      visit(repNode);

    assert(sccStack.empty() && "sccStack not empty after cycle detection!");
  }

  void releaseSCCMemory() {
    dfsNum.clear();
    inComponent.clear();
  }

public:
  CycleDetector() : timestamp(0) {}

  // The public interface of running the detector
  virtual void run() = 0;
};

#endif
