#include "Verification/Sifa/Procedure/ProcedureGraphBuilder.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instructions.h"

#include <queue>
#include <unordered_map>
#include <unordered_set>

using namespace lotus::sifa;

namespace {

using Node = ProcedureGraph::Node;

static llvm::SmallVector<Node, 2>
continuationTargets(const llvm::BasicBlock &BB, const llvm::CallBase *call) {
  llvm::SmallVector<Node, 2> out;
  if (const auto *invoke = llvm::dyn_cast<llvm::InvokeInst>(call)) {
    out.push_back(const_cast<llvm::BasicBlock *>(invoke->getNormalDest()));
    return out;
  }
  for (const llvm::BasicBlock *succ : llvm::successors(&BB)) {
    out.push_back(const_cast<llvm::BasicBlock *>(succ));
  }
  if (out.empty()) {
    out.push_back(nullptr);
  }
  return out;
}

} // namespace

ProcedureGraphBuilder::ProcedureGraphBuilder(SifaStats &stats, const llvm::Function &F)
    : stats_(stats), F_(F) {}

ProcedureGraph ProcedureGraphBuilder::graphOfProcedure(
    const std::vector<llvm::BasicBlock *> &locationsOfInterest,
    bool restrictToReachable) {
  return graphOfProcedure(locationsOfInterest, {}, restrictToReachable);
}

ProcedureGraph ProcedureGraphBuilder::graphOfProcedure(
    const std::vector<llvm::BasicBlock *> &locationsOfInterest,
    const std::vector<const llvm::Function *> &enterCallsOfInterest,
    bool restrictToReachable) {
  stats_.start(SifaStats::Key::PROCEDURE_GRAPH_BUILDER_TIME);
  struct StopTimer {
    SifaStats &stats;
    ~StopTimer() { stats.stop(SifaStats::Key::PROCEDURE_GRAPH_BUILDER_TIME); }
  } stopTimer{stats_};

  auto *entry = const_cast<llvm::BasicBlock *>(&F_.getEntryBlock());
  const Node exitNode = nullptr;

  if (enterCallsOfInterest.empty()) {
    if (!restrictToReachable || locationsOfInterest.empty()) {
      return ProcedureGraph(F_);
    }

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
        if (reachable.insert(pred).second) {
          work.push(pred);
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
        pg.addEdge(src, exitNode);
      }
      for (const llvm::Instruction &I : BB) {
        auto *call = llvm::dyn_cast<llvm::CallBase>(&I);
        if (!call) continue;
        llvm::Function *callee = call->getCalledFunction();
        if (!callee) continue;
        for (Node dst : continuationTargets(BB, call)) {
          if (reachable.count(dst)) {
            pg.addReturnSummaryEdge(src, dst, callee);
          }
        }
      }
    }
    return pg;
  }

  std::unordered_set<const llvm::Function *> enterCallSet(enterCallsOfInterest.begin(),
                                                          enterCallsOfInterest.end());
  std::unordered_map<const llvm::Function *, std::vector<Node>> enterCallPreds;
  std::unordered_map<Node, std::vector<Node>> summaryPreds;
  std::unordered_set<Node> callBlocksWithImplementedCallee;
  std::unordered_set<Node> requestedEnterTargets;

  for (const llvm::BasicBlock &BB : F_) {
    Node src = const_cast<llvm::BasicBlock *>(&BB);
    for (const llvm::Instruction &I : BB) {
      auto *call = llvm::dyn_cast<llvm::CallBase>(&I);
      if (!call) continue;
      llvm::Function *callee = call->getCalledFunction();
      if (!callee || callee->isDeclaration()) continue;

      callBlocksWithImplementedCallee.insert(src);
      for (Node dst : continuationTargets(BB, call)) {
        summaryPreds[dst].push_back(src);
      }
      if (enterCallSet.count(callee) && !callee->empty()) {
        Node calleeEntry = const_cast<llvm::BasicBlock *>(&callee->getEntryBlock());
        enterCallPreds[callee].push_back(src);
        requestedEnterTargets.insert(calleeEntry);
      }
    }
  }

  std::unordered_set<Node> reachable;
  std::queue<Node> work;
  work.push(exitNode);
  reachable.insert(exitNode);
  for (Node loi : locationsOfInterest) {
    if (loi && reachable.insert(loi).second) {
      work.push(loi);
    }
  }
  for (Node enterTarget : requestedEnterTargets) {
    if (reachable.insert(enterTarget).second) {
      work.push(enterTarget);
    }
  }

  while (!work.empty()) {
    Node cur = work.front();
    work.pop();
    if (!cur) {
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

    if (cur->getParent() != &F_) {
      const llvm::Function *callee = cur->getParent();
      auto it = enterCallPreds.find(callee);
      if (it == enterCallPreds.end()) {
        continue;
      }
      for (Node pred : it->second) {
        if (reachable.insert(pred).second) {
          work.push(pred);
        }
      }
      continue;
    }

    for (llvm::BasicBlock *pred : llvm::predecessors(cur)) {
      if (reachable.insert(pred).second) {
        work.push(pred);
      }
    }
    auto sumIt = summaryPreds.find(cur);
    if (sumIt != summaryPreds.end()) {
      for (Node pred : sumIt->second) {
        if (reachable.insert(pred).second) {
          work.push(pred);
        }
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

    const bool hasImplementedCall = callBlocksWithImplementedCallee.count(src);
    if (!hasImplementedCall) {
      for (const llvm::BasicBlock *SuccC : llvm::successors(&BB)) {
        Node dst = const_cast<llvm::BasicBlock *>(SuccC);
        if (reachable.count(dst)) {
          pg.addEdge(src, dst);
        }
      }
      if (llvm::succ_empty(&BB) && reachable.count(exitNode)) {
        pg.addEdge(src, exitNode);
      }
    }

    for (const llvm::Instruction &I : BB) {
      auto *call = llvm::dyn_cast<llvm::CallBase>(&I);
      if (!call) continue;
      llvm::Function *callee = call->getCalledFunction();
      if (!callee || callee->isDeclaration()) continue;

      for (Node dst : continuationTargets(BB, call)) {
        if (reachable.count(dst)) {
          pg.addReturnSummaryEdge(src, dst, callee);
        }
      }
      if (enterCallSet.count(callee)) {
        Node calleeEntry = callee->empty()
                               ? nullptr
                               : const_cast<llvm::BasicBlock *>(&callee->getEntryBlock());
        if (reachable.count(calleeEntry)) {
          pg.addEnterCallEdge(src, callee);
        }
      }
    }
  }

  return pg;
}
