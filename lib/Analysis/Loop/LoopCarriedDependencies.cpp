/*
 * Copyright 2026  Lotus contributors
 */
#include "Analysis/Loop/LoopCarriedDependencies.h"

namespace lotus {
namespace analysis {
namespace loop {

void LoopCarriedDependencies::setLoopCarriedDependencies(
    LoopTree *loopNode,
    const noelle::DominatorSummary &DS,
    LoopDependenceGraph &loopDG) {
  for (auto *edge : loopDG.getEdges()) {
    edge->setLoopCarried(false);
  }

  for (auto *edge : loopDG.getEdges()) {
    if (isALoopCarriedDependence(loopNode, DS, edge)) {
      edge->setLoopCarried(true);
    }
  }
}

bool LoopCarriedDependencies::isALoopCarriedDependence(
    LoopTree *loopNode,
    const noelle::DominatorSummary &DS,
    LoopDependenceEdge *edge) {
  auto *topLoop = loopNode->getLoop();
  assert(topLoop != nullptr);
  auto *topLoopHeader = topLoop->getHeader();
  auto *topLoopHeaderBranch = topLoopHeader->getTerminator();

  auto *producer = edge->getSrc()->getValue();
  auto *consumer = edge->getDst()->getValue();
  auto *producerI = dyn_cast_or_null<Instruction>(producer);
  auto *consumerI = dyn_cast_or_null<Instruction>(consumer);
  if (producerI == nullptr || consumerI == nullptr) {
    return false;
  }

  auto *producerLoop = loopNode->getInnermostLoopThatContains(producerI);
  auto *consumerLoop = loopNode->getInnermostLoopThatContains(consumerI);
  if (producerLoop == nullptr || consumerLoop == nullptr) {
    return false;
  }

  if (edge->getKind() == LoopDependenceEdgeKind::Control
      && producerLoop != loopNode->getLoop()
      && consumerLoop != loopNode->getLoop()) {
    return false;
  }

  bool sameElementSameIteration = true;
  if (edge->getKind() == LoopDependenceEdgeKind::Memory) {
    Value *producerPointer = nullptr;
    if (auto *load = dyn_cast<LoadInst>(producerI)) {
      producerPointer = load->getPointerOperand();
    } else if (auto *store = dyn_cast<StoreInst>(producerI)) {
      producerPointer = store->getPointerOperand();
    }

    Value *consumerPointer = nullptr;
    if (auto *load = dyn_cast<LoadInst>(consumerI)) {
      consumerPointer = load->getPointerOperand();
    } else if (auto *store = dyn_cast<StoreInst>(consumerI)) {
      consumerPointer = store->getPointerOperand();
    }

    if (producerPointer == nullptr || consumerPointer == nullptr) {
      sameElementSameIteration = false;
    } else if (producerPointer != consumerPointer) {
      sameElementSameIteration = false;
    } else if (auto *pointerInst = dyn_cast<Instruction>(producerPointer)) {
      if (topLoop->isIncluded(pointerInst)) {
        sameElementSameIteration = false;
      }
    } else {
      sameElementSameIteration = false;
    }
  }

  if (!sameElementSameIteration) {
    return true;
  }

  if (producerI == consumerI || !DS.DT.dominates(producerI, consumerI)) {
    if (edge->getKind() == LoopDependenceEdgeKind::Variable) {
      auto *producerBlock = producerI->getParent();
      auto *consumerBlock = consumerI->getParent();
      auto mustReachConsumerBeforeHeader = !canBasicBlockReachHeaderBeforeOther(
          *consumerLoop, producerBlock, consumerBlock);
      if (mustReachConsumerBeforeHeader) {
        return false;
      }

      if (DS.DT.dominates(consumerI, producerI)
          && DS.DT.dominates(topLoopHeaderBranch, consumerI)
          && isa<PHINode>(consumerI)) {
        return false;
      }
    }
    return true;
  }

  return false;
}

std::set<LoopDependenceEdge *>
LoopCarriedDependencies::getLoopCarriedDependenciesForLoop(
    const LoopStructure &loop,
    LoopTree *loopNode,
    LoopDependenceGraph &loopDG) {
  std::set<LoopDependenceEdge *> edges;
  for (auto *edge : loopDG.getEdges()) {
    if (!edge->isLoopCarried()) {
      continue;
    }
    auto *consumerI = dyn_cast_or_null<Instruction>(edge->getDst()->getValue());
    if (consumerI == nullptr) {
      continue;
    }
    auto *consumerLoop = loopNode->getInnermostLoopThatContains(consumerI);
    if (consumerLoop != &loop) {
      continue;
    }
    edges.insert(edge);
  }
  return edges;
}

std::set<LoopDependenceEdge *>
LoopCarriedDependencies::getLoopCarriedDependenciesForLoop(
    const LoopStructure &loop,
    LoopTree *loopNode,
    LoopSCCDAG &sccdag) {
  std::set<LoopDependenceEdge *> edges;
  auto *graph = sccdag.getLoopDependenceGraph();
  assert(graph != nullptr);
  for (auto *scc : sccdag.getSCCs()) {
    for (auto &pair : scc->internalNodePairs()) {
      auto *node = pair.second;
      for (auto *edge : node->getOutgoingEdges()) {
        if (!edge->isLoopCarried()) {
          continue;
        }
        auto *consumerI = dyn_cast_or_null<Instruction>(edge->getDst()->getValue());
        if (consumerI == nullptr) {
          continue;
        }
        auto *consumerLoop = loopNode->getInnermostLoopThatContains(consumerI);
        if (consumerLoop != &loop) {
          continue;
        }
        edges.insert(edge);
      }
    }
  }
  return edges;
}

bool LoopCarriedDependencies::canBasicBlockReachHeaderBeforeOther(
    const LoopStructure &loop,
    BasicBlock *from,
    BasicBlock *other) {
  assert(loop.isIncluded(from) && loop.isIncluded(other));
  if (from == other) {
    return true;
  }

  auto *header = loop.getHeader();
  auto exitsVector = loop.getLoopExitBasicBlocks();
  std::set<BasicBlock *> exits(exitsVector.begin(), exitsVector.end());
  std::queue<BasicBlock *> queue;
  std::unordered_set<BasicBlock *> enqueued;
  queue.push(from);
  enqueued.insert(from);
  bool reachedOther = false;

  while (!queue.empty()) {
    auto *current = queue.front();
    queue.pop();
    if (current == header) {
      return true;
    }
    if (exits.count(current) != 0) {
      continue;
    }
    if (current == other) {
      reachedOther = true;
      continue;
    }
    for (auto *succ : successors(current)) {
      if (!enqueued.insert(succ).second) {
        continue;
      }
      queue.push(succ);
    }
  }

  assert(reachedOther);
  return false;
}

} // namespace loop
} // namespace analysis
} // namespace lotus
