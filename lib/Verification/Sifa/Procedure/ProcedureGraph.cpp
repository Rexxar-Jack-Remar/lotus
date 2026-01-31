#include "Verification/Sifa/Procedure/ProcedureGraph.h"

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
  for (const llvm::BasicBlock &BB : F) {
    auto *src = const_cast<llvm::BasicBlock *>(&BB);
    graph_.addNode(src);
    for (const llvm::BasicBlock *SuccC : llvm::successors(&BB)) {
      auto *Succ = const_cast<llvm::BasicBlock *>(SuccC);
      const auto label = addTransition(src, Succ);
      graph_.addEdge(src, label, Succ);
    }

    if (llvm::succ_empty(&BB)) {
      // Explicit edge to EXIT sentinel (nullptr), as in SymbolicAbstraction's
      // location graph definition.
      const auto label = addTransition(src, /*dst=*/nullptr);
      graph_.addEdge(src, label, nullptr);
    }
  }

  // Interprocedural: add ReturnSummary edges for each call site (call/invoke).
  for (const llvm::BasicBlock &BB : F) {
    auto *src = const_cast<llvm::BasicBlock *>(&BB);
    for (const llvm::Instruction &I : BB) {
      auto *call = llvm::dyn_cast<llvm::CallBase>(&I);
      if (!call || !call->getCalledFunction()) continue;
      llvm::Function *callee = call->getCalledFunction();
      llvm::BasicBlock *normalSucc = nullptr;
      if (auto *invoke = llvm::dyn_cast<llvm::InvokeInst>(call))
        normalSucc = const_cast<llvm::BasicBlock *>(invoke->getNormalDest());
      else
        normalSucc = const_cast<llvm::BasicBlock *>(BB.getSingleSuccessor());
      if (callee && normalSucc)
        addReturnSummaryEdge(src, normalSucc, callee);
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
  const auto label = addTransition(src, dst);
  graph_.addEdge(src, label, dst);
}

void ProcedureGraph::addReturnSummaryEdge(Node src, Node dst, const llvm::Function *callee) {
  if (!src || !callee) return;
  const std::uint32_t id = static_cast<std::uint32_t>(transitions_.size());
  transitions_.push_back(
      TransitionInfo{src, const_cast<llvm::BasicBlock *>(dst), const_cast<llvm::Function *>(callee)});
  const auto label = Transition::makeReturnSummary(id, src, dst, callee);
  graph_.addEdge(src, label, dst);
}
