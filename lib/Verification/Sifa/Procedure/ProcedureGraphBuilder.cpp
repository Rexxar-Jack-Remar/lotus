#include "Verification/Sifa/Procedure/ProcedureGraphBuilder.h"

#include "llvm/IR/CFG.h"
#include "llvm/IR/Function.h"

#include <queue>
#include <unordered_set>

using namespace lotus::sifa;

ProcedureGraphBuilder::ProcedureGraphBuilder(SifaStats &stats, const llvm::Function &F)
    : stats_(stats), F_(F) {}

ProcedureGraph ProcedureGraphBuilder::graphOfProcedure(
    const std::vector<llvm::BasicBlock *> &locationsOfInterest,
    bool restrictToReachable) {
  auto *entry = const_cast<llvm::BasicBlock *>(&F_.getEntryBlock());
  const Node exitNode = nullptr; // single exit sentinel

  if (!restrictToReachable || locationsOfInterest.empty()) {
    return ProcedureGraph(F_);
  }

  // Backward BFS from exit and LOIs to get backward-reachable set.
  std::unordered_set<Node> reachable;
  std::queue<Node> work;
  work.push(exitNode);
  reachable.insert(exitNode);
  for (Node loi : locationsOfInterest) {
    if (loi && reachable.insert(loi).second) {
      work.push(loi);
    }
  }

  while (!work.empty()) {
    Node cur = work.front();
    work.pop();
    if (!cur) {
      // Predecessors of exit: all blocks with no successors (ret/br to exit).
      for (const llvm::BasicBlock &BB : F_) {
        if (llvm::succ_empty(&BB)) {
          Node pred = const_cast<llvm::BasicBlock *>(&BB);
          if (reachable.insert(pred).second) {
            work.push(pred);
          }
        }
      }
      continue;
    }
    for (llvm::BasicBlock *pred : llvm::predecessors(cur)) {
      Node p = pred;
      if (reachable.insert(p).second) {
        work.push(p);
      }
    }
  }

  ProcedureGraph pg(entry, exitNode);
  for (Node n : reachable) {
    pg.addNode(n);
  }
  for (const llvm::BasicBlock &BB : F_) {
    Node src = const_cast<llvm::BasicBlock *>(&BB);
    if (!reachable.count(src)) {
      continue;
    }
    for (const llvm::BasicBlock *SuccC : llvm::successors(&BB)) {
      Node dst = const_cast<llvm::BasicBlock *>(SuccC);
      if (reachable.count(dst)) {
        pg.addEdge(src, dst);
      }
    }
    if (llvm::succ_empty(&BB) && reachable.count(nullptr)) {
      pg.addEdge(src, nullptr);
    }
  }
  // ProcedureGraph::addEdge(src, nullptr) - we need to support edge to exit.
  // Currently addEdge does if (!src || !dst) return; so dst=nullptr is skipped.
  // So we must allow nullptr in reachable and add edges to exit. So we need
  // ProcedureGraph to support adding an edge (src, nullptr). Let me check
  // addEdge again - we have addEdge(Node src, Node dst). If dst is nullptr,
  // we skip. So we need to either allow dst=nullptr in addEdge or add a
  // separate addEdgeToExit. In ProcedureGraph full constructor we do
  // graph_.addEdge(src, label, nullptr). So the graph can have nullptr as
  // target. So we need addEdge to allow dst=nullptr. Let me update addEdge.
  if (reachable.count(nullptr)) {
    for (const llvm::BasicBlock &BB : F_) {
      if (llvm::succ_empty(&BB)) {
        Node src = const_cast<llvm::BasicBlock *>(&BB);
        if (reachable.count(src)) {
          pg.addEdge(src, nullptr);
        }
      }
    }
  }

  return pg;
}
