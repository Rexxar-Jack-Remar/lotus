#include "Verification/Sifa/Procedure/ProcedureGraph.h"

#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"

#include <cstddef>
#include <functional>
#include <utility>

using namespace lotus::sifa;

std::size_t ProcedureGraph::NodePairHash::operator()(
    const std::pair<Node, Node> &p) const {
  const std::size_t a = std::hash<Node>()(p.first);
  const std::size_t b = std::hash<Node>()(p.second);
  return a ^ (b + 0x9e3779b97f4a7c15ULL + (a << 6) + (a >> 2));
}

ProcedureGraph::Node ProcedureGraph::getEntryNode() const { return entryNode_; }
ProcedureGraph::Node ProcedureGraph::getExitNode() const { return exitNode_; }
const ProcedureGraph::Graph &ProcedureGraph::graph() const { return graph_; }
const std::vector<TransitionInfo> &ProcedureGraph::transitions() const { return transitions_; }

Transition ProcedureGraph::addTransition(Node src, Node dst) {
  const std::pair<Node, Node> k{src, dst};
  const auto it = edgeToId_.find(k);
  if (it != edgeToId_.end()) {
    return Transition::makeEdge(it->second, src, dst);
  }

  const std::uint32_t id = static_cast<std::uint32_t>(transitions_.size());
  transitions_.push_back(TransitionInfo{src, dst});
  edgeToId_.emplace(std::move(k), id);
  return Transition::makeEdge(id, src, dst);
}

ProcedureGraph::ProcedureGraph(const llvm::Function &F) {
  // Collect blocks that contain an InvokeInst with a resolved callee so we can
  // skip adding a plain CFG edge for the invoke's normal-dest successor.
  // The ReturnSummary edge added in the second pass already covers that path;
  // adding both would create a duplicate path through the normal-dest block.
  llvm::SmallPtrSet<const llvm::BasicBlock *, 8> invokeBlocks;
  for (const llvm::BasicBlock &BB : F) {
    for (const llvm::Instruction &I : BB) {
      if (const auto *invoke = llvm::dyn_cast<llvm::InvokeInst>(&I)) {
        if (invoke->getCalledFunction()) {
          invokeBlocks.insert(&BB);
          break;
        }
      }
    }
  }

  // Regular intraprocedural CFG edges. For blocks with no successors we add a
  // synthetic outgoing edge to the EXIT sentinel (nullptr).
  // For invoke blocks with a resolved callee, skip the normal-dest edge here;
  // it will be represented by a ReturnSummary edge below.
  for (const llvm::BasicBlock &BB : F) {
    auto *src = const_cast<llvm::BasicBlock *>(&BB);
    graph_.addNode(src);

    if (invokeBlocks.count(&BB)) {
      // Find the invoke and its normal/unwind dests.
      for (const llvm::Instruction &I : BB) {
        if (const auto *invoke = llvm::dyn_cast<llvm::InvokeInst>(&I)) {
          if (invoke->getCalledFunction()) {
            // Only add the unwind edge as a plain CFG edge; the normal-dest
            // is covered by the ReturnSummary edge added below.
            auto *unwind = const_cast<llvm::BasicBlock *>(invoke->getUnwindDest());
            const auto label = addTransition(src, unwind);
            graph_.addEdge(src, label, unwind);
            break;
          }
        }
      }
    } else {
      for (const llvm::BasicBlock *SuccC : llvm::successors(&BB)) {
        auto *Succ = const_cast<llvm::BasicBlock *>(SuccC);
        const auto label = addTransition(src, Succ);
        graph_.addEdge(src, label, Succ);
      }
    }

    if (llvm::succ_empty(&BB)) {
      // Explicit edge to EXIT sentinel (nullptr). This makes the CFG "single-exit"
      // in the sense that all returns flow into one sink, which simplifies
      // construction of path expressions.
      const auto label = addTransition(src, /*dst=*/nullptr);
      graph_.addEdge(src, label, nullptr);
    }
  }

  // Interprocedural aid: add ReturnSummary edges for each direct call site.
  //
  // A ReturnSummary is a synthetic "call + execute callee + return" transition
  // (Ultimate's CallReturnSummary). This keeps the path-expression alphabet
  // small while allowing ICFG-style interpretation to account for calls.
  //
  // - We add these edges only for direct calls where we can resolve the callee.
  // - The edge goes from the block containing the call to the normal successor.
  // - Indirect calls are ignored here and must be handled conservatively by
  //   the interpreter/domain if interprocedural semantics are needed.
  // - For InvokeInst, the normal-dest edge is represented solely by this
  //   ReturnSummary (the plain CFG edge was skipped above).
  for (const llvm::BasicBlock &BB : F) {
    auto *src = const_cast<llvm::BasicBlock *>(&BB);
    for (const llvm::Instruction &I : BB) {
      auto *call = llvm::dyn_cast<llvm::CallBase>(&I);
      if (!call) continue;
      llvm::Function *callee = call->getCalledFunction();
      if (!callee) continue;
      llvm::BasicBlock *normalSucc = nullptr;
      if (auto *invoke = llvm::dyn_cast<llvm::InvokeInst>(call)) {
        normalSucc = const_cast<llvm::BasicBlock *>(invoke->getNormalDest());
      } else {
        // Non-invoke calls should have exactly one successor (fallthrough).
        normalSucc = const_cast<llvm::BasicBlock *>(BB.getSingleSuccessor());
      }
      if (normalSucc) {
        addReturnSummaryEdge(src, normalSucc, callee);
      }
    }
  }

  entryNode_ = const_cast<llvm::BasicBlock *>(&F.getEntryBlock());
  exitNode_ = nullptr; // multiple exits represented by edges to nullptr
}

ProcedureGraph::ProcedureGraph(Node entryNode, Node exitNode)
    : entryNode_(entryNode), exitNode_(exitNode) {
  if (entryNode_) {
    graph_.addNode(entryNode_);
  }
  if (exitNode_ && exitNode_ != entryNode_) {
    graph_.addNode(exitNode_);
  }
}

void ProcedureGraph::addNode(Node n) {
  if (n) {
    graph_.addNode(n);
  }
}

void ProcedureGraph::addEdge(Node src, Node dst) {
  if (!src) {
    return;
  }
  // Note: dst may be nullptr to represent an edge to the EXIT sentinel.
  const auto label = addTransition(src, dst);
  graph_.addEdge(src, label, dst);
}

void ProcedureGraph::addReturnSummaryEdge(Node src, Node dst, const llvm::Function *callee) {
  if (!src || !dst || !callee) return;
  // ReturnSummary transitions are not de-duplicated by (src,dst) because the
  // callee identity is part of the semantics. Each added edge gets a fresh id.
  const std::uint32_t id = static_cast<std::uint32_t>(transitions_.size());
  transitions_.push_back(
      TransitionInfo{src, const_cast<llvm::BasicBlock *>(dst), const_cast<llvm::Function *>(callee)});
  const auto label = Transition::makeReturnSummary(id, src, dst, callee);
  graph_.addEdge(src, label, dst);
}
