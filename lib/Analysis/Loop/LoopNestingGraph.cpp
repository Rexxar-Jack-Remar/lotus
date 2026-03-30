/*
 * Copyright 2026  Lotus contributors
 */
#include "Analysis/Loop/LoopNestingGraph.h"

namespace lotus {
namespace analysis {
namespace loop {

std::set<LoopNestingGraphEdge *> LoopNestingGraphLoopNode::getIncomingEdges(void) const {
  return this->incomingEdges;
}

std::set<LoopNestingGraphEdge *> LoopNestingGraphLoopNode::getOutgoingEdges(void) const {
  return this->outgoingEdges;
}

void LoopNestingGraphLoopNode::addIncomingEdge(LoopNestingGraphEdge *edge) {
  this->incomingEdges.insert(edge);
}

void LoopNestingGraphLoopNode::addOutgoingEdge(LoopNestingGraphEdge *edge) {
  this->outgoingEdges.insert(edge);
}

LoopNestingGraphLoopLoopEdge *
LoopNestingGraphLoopNode::getNestingEdgeTo(LoopNestingGraphLoopNode *target) const {
  for (auto *edge : this->outgoingEdges) {
    auto *loopEdge = dynamic_cast<LoopNestingGraphLoopLoopEdge *>(edge);
    if (loopEdge != nullptr && loopEdge->getDst() == target) {
      return loopEdge;
    }
  }
  return nullptr;
}

void LoopNestingGraphLoopLoopEdge::addSubEdge(
    LoopNestingGraphInstructionLoopEdge *edge) {
  this->subEdges.push_back(edge);
}

std::vector<LoopNestingGraphInstructionLoopEdge *>
LoopNestingGraphLoopLoopEdge::getSubEdges(void) const {
  return this->subEdges;
}

LoopNestingGraph::LoopNestingGraph(std::vector<LoopStructure *> const &loops) {
  for (auto *loop : loops) {
    this->loops[loop] = new LoopNestingGraphLoopNode(loop);
  }
}

std::unordered_set<LoopNestingGraphLoopNode *> LoopNestingGraph::getLoopNodes(void) const {
  std::unordered_set<LoopNestingGraphLoopNode *> nodes;
  for (auto &pair : this->loops) {
    nodes.insert(pair.second);
  }
  return nodes;
}

std::unordered_set<LoopNestingGraphEdge *> LoopNestingGraph::getEdges(void) const {
  std::unordered_set<LoopNestingGraphEdge *> edgeSet;
  for (auto &pair : this->edges) {
    for (auto *edge : pair.second) {
      edgeSet.insert(edge);
    }
  }
  return edgeSet;
}

LoopNestingGraphLoopNode *LoopNestingGraph::getLoopNode(LoopStructure *loop) const {
  auto it = this->loops.find(loop);
  if (it == this->loops.end()) {
    return nullptr;
  }
  return it->second;
}

LoopNestingGraphLoopNode *LoopNestingGraph::getEntryNode(Function *entryFunction) const {
  for (auto &pair : this->loops) {
    if (pair.first->getFunction() != entryFunction) {
      continue;
    }
    if (pair.second->getIncomingEdges().empty()) {
      return pair.second;
    }
  }
  return nullptr;
}

void LoopNestingGraph::createEdge(LoopStructure *from,
                                  CallBase *callInst,
                                  LoopStructure *child,
                                  bool isMust) {
  auto fromIt = this->loops.find(from);
  assert(fromIt != this->loops.end());
  this->fetchOrCreateEdge(fromIt->second, callInst, child, isMust);
}

LoopNestingGraphLoopLoopEdge *LoopNestingGraph::fetchOrCreateEdge(
    LoopNestingGraphLoopNode *fromNode,
    CallBase *callInst,
    LoopStructure *child,
    bool isMust) {
  auto *toNode = this->loops.at(child);

  LoopNestingGraphInstructionNode *instNode = nullptr;
  if (callInst != nullptr) {
    auto &slot = this->instructionNodes[callInst];
    if (slot == nullptr) {
      slot = new LoopNestingGraphInstructionNode(callInst);
    }
    instNode = slot;
  }

  auto *subEdge = new LoopNestingGraphInstructionLoopEdge(instNode, toNode, isMust);
  auto *existing = fromNode->getNestingEdgeTo(toNode);
  if (existing == nullptr) {
    auto *newEdge = new LoopNestingGraphLoopLoopEdge(fromNode, toNode, isMust);
    this->edges[fromNode].insert(newEdge);
    fromNode->addOutgoingEdge(newEdge);
    toNode->addIncomingEdge(newEdge);
    newEdge->addSubEdge(subEdge);
    return newEdge;
  }

  if (isMust) {
    existing->setMust();
  }
  existing->addSubEdge(subEdge);
  return existing;
}

std::unique_ptr<LoopNestingGraph> LoopNestingGraph::buildFromAnalyses(
    std::vector<FunctionLoopAnalyses *> const &analyses,
    llvm::Module &module,
    Function *entryFunction) {
  std::vector<LoopStructure *> allLoops;
  std::unordered_map<const Function *, FunctionLoopAnalyses *> analysesByFunction;
  for (auto *analysis : analyses) {
    analysesByFunction[analysis->getFunction()] = analysis;
    auto loops = analysis->getLoopStructures();
    allLoops.insert(allLoops.end(), loops.begin(), loops.end());
  }

  auto graph = std::unique_ptr<LoopNestingGraph>(new LoopNestingGraph(allLoops));

  for (auto *analysis : analyses) {
    auto *forest = analysis->getLoopForest();
    if (forest == nullptr) {
      continue;
    }
    for (auto *tree : forest->getTrees()) {
      tree->visitPreOrder([&](LoopTree *node, uint32_t) -> bool {
        if (node->getParent() != nullptr) {
          graph->createEdge(node->getParent()->getLoop(),
                            nullptr,
                            node->getLoop(),
                            true);
        }
        return false;
      });
    }
  }

  std::map<const Function *, std::unordered_set<LoopStructure *>> outermostLoops;
  for (auto *loop : allLoops) {
    if (loop->getNestingLevel() == 1) {
      outermostLoops[loop->getFunction()].insert(loop);
    }
  }

  auto &callGraph = pdg::PDGCallGraph::getInstance();
  if (!callGraph.isBuiltForModule(module)) {
    callGraph.reset();
    callGraph.build(module);
  }

  for (auto *analysis : analyses) {
    auto *callerFunction = analysis->getFunction();
    auto *callerForest = analysis->getLoopForest();
    if (callerFunction == nullptr || callerForest == nullptr) {
      continue;
    }

    for (auto &bb : *callerFunction) {
      for (auto &inst : bb) {
        auto *callBase = dyn_cast<CallBase>(&inst);
        if (callBase == nullptr) {
          continue;
        }

        auto *loopNode = callerForest->getInnermostLoopThatContains(callBase);
        if (loopNode == nullptr) {
          continue;
        }
        auto *parentLoop = loopNode->getLoop();

        if (auto *callee = callBase->getCalledFunction()) {
          auto it = outermostLoops.find(callee);
          if (it == outermostLoops.end()) {
            continue;
          }
          for (auto *outermostLoop : it->second) {
            graph->createEdge(parentLoop, callBase, outermostLoop, true);
          }
          continue;
        }

        auto candidates = callGraph.getIndirectCallCandidates(*callBase, module);
        for (auto *callee : candidates) {
          auto it = outermostLoops.find(callee);
          if (it == outermostLoops.end()) {
            continue;
          }
          for (auto *outermostLoop : it->second) {
            graph->createEdge(parentLoop, callBase, outermostLoop, false);
          }
        }
      }
    }
  }

  (void)graph->getEntryNode(entryFunction);
  return graph;
}

} // namespace loop
} // namespace analysis
} // namespace lotus
