/*
 * Copyright 2026 Lotus contributors
 */
#include "Analysis/Loop/LoopContent.h"

#include <queue>
#include <unordered_set>

namespace lotus {
namespace analysis {
namespace loop {

LoopContent::LoopContent(LoopTree *loopNode) : loop{loopNode} {}

void LoopContent::invalidateDerivedAnalyses(void) {
  this->ivDependenceGraph.reset();
  this->ivSCCDAG.reset();
  this->environment.reset();
  this->invariantManager.reset();
  this->inductionVariables.reset();
  this->iterationSpaceAnalysis.reset();
  this->memoryCloningAnalysis.reset();
  this->sccAttrs.reset();
}

void LoopContent::rebuildSCCDAG(void) {
  assert(this->dependenceGraph != nullptr);
  this->sccdag.reset(new LoopSCCDAG(*this->dependenceGraph));
}

void LoopContent::materializeDependenceGraph(pdg::ProgramGraph &pdg) {
  this->dependenceGraph.reset(new LoopDependenceGraph(this->loop, pdg));
  this->rebuildSCCDAG();
  this->invalidateDerivedAnalyses();
}

void LoopContent::buildEnvironment(const std::set<Value *> &excludeValues) {
  assert(this->dependenceGraph != nullptr);
  this->environment.reset(new LoopEnvironment(
      this->dependenceGraph.get(),
      this->getLoopStructure()->getLoopExitBasicBlocks(),
      excludeValues));
}

void LoopContent::materializeEnvironment(void) {
  if (this->environment != nullptr) {
    return;
  }
  this->buildEnvironment({});
}

void LoopContent::materializeLoopCarriedDependencies(
    const noelle::DominatorSummary &DS) {
  assert(this->dependenceGraph != nullptr);
  LoopCarriedDependencies::setLoopCarriedDependencies(
      this->loop, DS, *this->dependenceGraph);
}

bool LoopContent::canInstructionReachWithinSameIteration(Instruction *from,
                                                         Instruction *to) const {
  auto *loopStructure = this->getLoopStructure();
  if (loopStructure == nullptr || from == nullptr || to == nullptr) {
    return false;
  }
  if (!loopStructure->isIncluded(from) || !loopStructure->isIncluded(to)) {
    return false;
  }

  if (from->getParent() == to->getParent()) {
    bool seenFrom = false;
    for (auto &inst : *from->getParent()) {
      if (&inst == from) {
        seenFrom = true;
      }
      if (seenFrom && &inst == to) {
        return true;
      }
    }
    return false;
  }

  std::queue<BasicBlock *> worklist;
  std::unordered_set<BasicBlock *> visited;
  worklist.push(from->getParent());
  visited.insert(from->getParent());

  while (!worklist.empty()) {
    auto *current = worklist.front();
    worklist.pop();
    for (auto *succ : successors(current)) {
      if (!loopStructure->isIncluded(succ)) {
        continue;
      }
      if (succ == to->getParent()) {
        return true;
      }
      if (visited.insert(succ).second) {
        worklist.push(succ);
      }
    }
  }

  return false;
}

void LoopContent::removeLoopCarriedDependencesProvedDisjoint(
    LoopIterationSpaceAnalysis &analysis) {
  assert(this->dependenceGraph != nullptr);

  std::vector<LoopDependenceEdge *> edgesToRemove;
  for (auto *edge : this->dependenceGraph->getEdges()) {
    if (!edge->isLoopCarried() || edge->getKind() != LoopDependenceEdgeKind::Memory) {
      continue;
    }

    auto *from = dyn_cast_or_null<Instruction>(edge->getSrc()->getValue());
    auto *to = dyn_cast_or_null<Instruction>(edge->getDst()->getValue());
    if (from == nullptr || to == nullptr) {
      continue;
    }
    if (this->canInstructionReachWithinSameIteration(from, to)) {
      continue;
    }
    if (!analysis.areInstructionsAccessingDisjointMemoryLocationsBetweenIterations(
            from, to)) {
      continue;
    }

    edge->setLoopCarried(false);
    edgesToRemove.push_back(edge);
  }

  for (auto *edge : edgesToRemove) {
    this->dependenceGraph->removeEdge(edge);
  }
}

void LoopContent::removeMemoryCloningNegatedDependences(void) {
  if (this->dependenceGraph == nullptr || this->memoryCloningAnalysis == nullptr) {
    return;
  }

  std::vector<LoopDependenceEdge *> edgesToRemove;
  for (auto *edge : this->dependenceGraph->getEdges()) {
    if (!edge->isLoopCarried() || edge->getKind() != LoopDependenceEdgeKind::Memory) {
      continue;
    }

    auto *producer = dyn_cast_or_null<Instruction>(edge->getSrc()->getValue());
    auto *consumer = dyn_cast_or_null<Instruction>(edge->getDst()->getValue());
    if (producer == nullptr || consumer == nullptr) {
      continue;
    }

    auto locationsProducer =
        this->memoryCloningAnalysis->getClonableMemoryObjectsFor(producer);
    auto locationsConsumer =
        this->memoryCloningAnalysis->getClonableMemoryObjectsFor(consumer);
    if (locationsProducer.empty() || locationsConsumer.empty()) {
      continue;
    }

    bool removable = false;
    for (auto *locationProducer : locationsProducer) {
      for (auto *locationConsumer : locationsConsumer) {
        if (locationProducer->getAllocation() != locationConsumer->getAllocation()) {
          continue;
        }
        bool producerTouchesMemory =
            locationProducer->isInstructionStoringLocation(producer)
            || locationProducer->isInstructionLoadingLocation(producer);
        bool consumerTouchesMemory =
            locationConsumer->isInstructionStoringLocation(consumer)
            || locationConsumer->isInstructionLoadingLocation(consumer);
        if (!producerTouchesMemory || !consumerTouchesMemory) {
          continue;
        }
        if (locationProducer->isInstructionStoringLocation(producer)
            || locationConsumer->isInstructionStoringLocation(consumer)) {
          removable = true;
          break;
        }
      }
      if (removable) {
        break;
      }
    }

    if (!removable) {
      continue;
    }

    edge->setLoopCarried(false);
    edgesToRemove.push_back(edge);
  }

  for (auto *edge : edgesToRemove) {
    this->dependenceGraph->removeEdge(edge);
  }
}

void LoopContent::materializeScalarAnalyses(llvm::ScalarEvolution &SE,
                                            llvm::LoopInfo &LI,
                                            const noelle::DominatorSummary &DS) {
  assert(this->dependenceGraph != nullptr);
  assert(this->sccdag != nullptr);

  this->invalidateDerivedAnalyses();

  LoopCarriedDependencies::setLoopCarriedDependencies(
      this->loop, DS, *this->dependenceGraph);

  InvariantManager temporaryInvariants(this->getLoopStructure(),
                                       this->dependenceGraph.get());
  auto temporaryIVDependenceGraph =
      this->dependenceGraph->createSubgraph(/*includeControl=*/true,
                                            /*includeVariable=*/true,
                                            /*includeMemory=*/false);
  auto temporaryIVSCCDAG =
      std::unique_ptr<LoopSCCDAG>(new LoopSCCDAG(*temporaryIVDependenceGraph));
  auto temporaryIVs = std::unique_ptr<InductionVariableManager>(
      new InductionVariableManager(this->loop,
                                   temporaryInvariants,
                                   SE,
                                   LI,
                                   *temporaryIVSCCDAG));
  LoopIterationSpaceAnalysis temporaryIterationSpace(
      this->loop, *temporaryIVs, SE);
  this->removeLoopCarriedDependencesProvedDisjoint(temporaryIterationSpace);

  LoopCarriedDependencies::setLoopCarriedDependencies(
      this->loop, DS, *this->dependenceGraph);
  this->memoryCloningAnalysis.reset(new MemoryCloningAnalysis(
      this->getLoopStructure(), const_cast<noelle::DominatorSummary &>(DS),
      this->dependenceGraph.get()));

  std::vector<Instruction *> clonableAccesses;
  for (auto *instruction : this->getLoopStructure()->getInstructions()) {
    bool isMemoryTraffic = isa<LoadInst>(instruction) || isa<StoreInst>(instruction);
    if (!isMemoryTraffic) {
      if (auto *callInst = dyn_cast<CallInst>(instruction)) {
        isMemoryTraffic = !callInst->isLifetimeStartOrEnd();
      }
    }
    if (!isMemoryTraffic) {
      continue;
    }
    if (!this->memoryCloningAnalysis->getClonableMemoryObjectsFor(instruction).empty()) {
      clonableAccesses.push_back(instruction);
    }
  }
  for (auto *src : clonableAccesses) {
    auto srcLocations = this->memoryCloningAnalysis->getClonableMemoryObjectsFor(src);
    for (auto *dst : clonableAccesses) {
      if (src == dst) {
        continue;
      }
      auto dstLocations = this->memoryCloningAnalysis->getClonableMemoryObjectsFor(dst);
      bool sharesLocation = false;
      for (auto *srcLocation : srcLocations) {
        for (auto *dstLocation : dstLocations) {
          if (srcLocation->getAllocation() == dstLocation->getAllocation()) {
            sharesLocation = true;
            break;
          }
        }
        if (sharesLocation) {
          break;
        }
      }
      if (sharesLocation) {
        this->dependenceGraph->addVariableDependence(src, dst);
      }
    }
  }
  this->removeMemoryCloningNegatedDependences();

  this->rebuildSCCDAG();
  LoopCarriedDependencies::setLoopCarriedDependencies(
      this->loop, DS, *this->dependenceGraph);

  std::set<Value *> excludeValues;
  for (auto *memoryObject : this->memoryCloningAnalysis->getClonableMemoryObjects()) {
    if (!memoryObject->doPrivateCopiesNeedToBeInitialized()) {
      excludeValues.insert(memoryObject->getAllocation());
    }
  }
  this->buildEnvironment(excludeValues);

  this->invariantManager.reset(
      new InvariantManager(this->getLoopStructure(), this->dependenceGraph.get()));
  this->ivDependenceGraph =
      this->dependenceGraph->createSubgraph(/*includeControl=*/true,
                                            /*includeVariable=*/true,
                                            /*includeMemory=*/false);
  this->ivSCCDAG.reset(new LoopSCCDAG(*this->ivDependenceGraph));
  this->inductionVariables.reset(new InductionVariableManager(
      this->loop, *this->invariantManager, SE, LI, *this->ivSCCDAG));
}

void LoopContent::materializeIterationSpaceAnalysis(llvm::ScalarEvolution &SE) {
  assert(this->inductionVariables != nullptr);
  this->iterationSpaceAnalysis.reset(
      new LoopIterationSpaceAnalysis(this->loop, *this->inductionVariables, SE));
}

void LoopContent::materializeSCCAttrs(bool enableFloatAsReal,
                                      noelle::DominatorSummary &DS) {
  assert(this->dependenceGraph != nullptr);
  assert(this->sccdag != nullptr);
  assert(this->inductionVariables != nullptr);
  if (this->memoryCloningAnalysis == nullptr) {
    this->memoryCloningAnalysis.reset(new MemoryCloningAnalysis(
        this->getLoopStructure(), DS, this->dependenceGraph.get()));
  }
  this->sccAttrs.reset(new SCCDAGAttrs(enableFloatAsReal,
                                       this->dependenceGraph.get(),
                                       this->sccdag.get(),
                                       this->loop,
                                       *this->inductionVariables,
                                       DS));
}

} // namespace loop
} // namespace analysis
} // namespace lotus
