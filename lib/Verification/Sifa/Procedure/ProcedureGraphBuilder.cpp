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
  stats_.start(SifaStats::Key::PROCEDURE_GRAPH_BUILDER_TIME);
  struct StopTimer {
    SifaStats &stats;
    ~StopTimer() { stats.stop(SifaStats::Key::PROCEDURE_GRAPH_BUILDER_TIME); }
  } stopTimer{stats_};

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
    if (llvm::succ_empty(&BB) && reachable.count(exitNode)) {
      // Preserve exit edges for return blocks if EXIT is in the restricted slice.
      pg.addEdge(src, exitNode);
    }
  }

  return pg;
}
