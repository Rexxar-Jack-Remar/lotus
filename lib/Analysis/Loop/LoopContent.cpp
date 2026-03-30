/*
 * Copyright 2026 Lotus contributors
 */
#include "Analysis/Loop/LoopContent.h"

#include "Alias/Spec/AliasSpecManager.h"

#include <queue>
#include <unordered_set>

namespace lotus {
namespace analysis {
namespace loop {

namespace {

bool canInstructionReachWithinSameIteration(const LoopStructure *loopStructure,
                                            Instruction *from,
                                            Instruction *to) {
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
  auto *header = loopStructure->getHeader();

  while (!worklist.empty()) {
    auto *current = worklist.front();
    worklist.pop();
    for (auto *succ : successors(current)) {
      if (!loopStructure->isIncluded(succ) || succ == header) {
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

void removeLoopCarriedDependencesProvedDisjoint(
    LoopDependenceGraph &graph, LoopStructure *loopStructure,
    LoopIterationSpaceAnalysis &analysis) {
  std::vector<LoopDependenceEdge *> edgesToRemove;
  for (auto *edge : graph.getEdges()) {
    if (!edge->isLoopCarried() ||
        edge->getKind() != LoopDependenceEdgeKind::Memory) {
      continue;
    }

    auto *from = dyn_cast_or_null<Instruction>(edge->getSrc()->getValue());
    auto *to = dyn_cast_or_null<Instruction>(edge->getDst()->getValue());
    if (from == nullptr || to == nullptr) {
      continue;
    }
    if (canInstructionReachWithinSameIteration(loopStructure, from, to)) {
      continue;
    }
    if (!analysis
             .areInstructionsAccessingDisjointMemoryLocationsBetweenIterations(
                 from, to)) {
      continue;
    }

    edge->setLoopCarried(false);
    edgesToRemove.push_back(edge);
  }

  for (auto *edge : edgesToRemove) {
    graph.removeEdge(edge);
  }
}

class LoopDependenceRefinementPass {
public:
  virtual ~LoopDependenceRefinementPass() = default;

  virtual void refine(LoopContent &content, llvm::ScalarEvolution &SE,
                      llvm::LoopInfo &LI,
                      const noelle::DominatorSummary &DS) = 0;
};

class UnsupportedLoopAwareDependenceRefinementPass
    : public LoopDependenceRefinementPass {
public:
  void refine(LoopContent &, llvm::ScalarEvolution &, llvm::LoopInfo &,
              const noelle::DominatorSummary &) override {
    // NOELLE can host additional loop-aware disproval engines such as SCAF.
    // Lotus preserves the stage boundary here even when no extra engine is
    // available yet.
  }
};

class AffineLoopDependenceRefinementPass : public LoopDependenceRefinementPass {
public:
  void refine(LoopContent &content, llvm::ScalarEvolution &SE, llvm::LoopInfo &LI,
              const noelle::DominatorSummary &DS) override {
    InvariantManager temporaryInvariants(content.getLoopStructure(),
                                         content.getLoopDependenceGraph());
    auto temporaryIVDependenceGraph =
        content.getLoopDependenceGraph()->createSubgraph(
            /*includeControl=*/true,
            /*includeVariable=*/true,
            /*includeMemory=*/false);
    auto temporaryIVSCCDAG =
        std::unique_ptr<LoopSCCDAG>(new LoopSCCDAG(*temporaryIVDependenceGraph));
    LoopEnvironment temporaryEnvironment(
        content.getLoopDependenceGraph(),
        content.getLoopStructure()->getLoopExitBasicBlocks());
    auto temporaryIVs = std::unique_ptr<InductionVariableManager>(
        new InductionVariableManager(content.getLoopHierarchyStructures(),
                                     temporaryInvariants, SE, LI,
                                     *temporaryIVSCCDAG,
                                     temporaryEnvironment));
    LoopIterationSpaceAnalysis temporaryIterationSpace(
        content.getLoopHierarchyStructures(), *temporaryIVs, SE);
    removeLoopCarriedDependencesProvedDisjoint(
        *content.getLoopDependenceGraph(), content.getLoopStructure(),
        temporaryIterationSpace);
    LoopCarriedDependencies::setLoopCarriedDependencies(
        content.getLoopHierarchyStructures(), DS,
        *content.getLoopDependenceGraph());
  }
};

lotus::alias::AliasSpecManager &getLoopAnalysisSpecManager(void) {
  static lotus::alias::AliasSpecManager manager;
  return manager;
}

bool isThreadSafeLibraryCall(CallBase *call) {
  if (call == nullptr) {
    return false;
  }
  auto *callee = call->getCalledFunction();
  if (callee == nullptr || !callee->empty()) {
    return false;
  }

  auto &specManager = getLoopAnalysisSpecManager();
  if (specManager.isAllocator(callee) || specManager.isDeallocator(callee) ||
      specManager.getCategory(callee) ==
          lotus::alias::FunctionCategory::Reallocator) {
    return true;
  }

  static const std::set<std::string> noelleThreadSafeFallback{
      "malloc", "calloc", "realloc", "free"};
  return noelleThreadSafeFallback.count(callee->getName().str()) != 0;
}

} // namespace

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
      this->getLoopStructure()->getLoopExitBasicBlocks(), excludeValues));
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
  LoopCarriedDependencies::setLoopCarriedDependencies(this->loop, DS,
                                                      *this->dependenceGraph);
}

bool LoopContent::canInstructionReachWithinSameIteration(
    Instruction *from, Instruction *to) const {
  return ::lotus::analysis::loop::canInstructionReachWithinSameIteration(
      this->getLoopStructure(), from, to);
}

void LoopContent::removeLoopCarriedDependencesProvedDisjoint(
    LoopIterationSpaceAnalysis &analysis) {
  assert(this->dependenceGraph != nullptr);
  ::lotus::analysis::loop::removeLoopCarriedDependencesProvedDisjoint(
      *this->dependenceGraph, this->getLoopStructure(), analysis);
}

void LoopContent::removeThreadSafeLibraryDependences(void) {
  assert(this->dependenceGraph != nullptr);

  std::vector<LoopDependenceEdge *> edgesToRemove;
  for (auto *edge : this->dependenceGraph->getEdges()) {
    if (!edge->isLoopCarried() ||
        edge->getKind() != LoopDependenceEdgeKind::Memory) {
      continue;
    }

    auto *producer = dyn_cast_or_null<Instruction>(edge->getSrc()->getValue());
    auto *consumer = dyn_cast_or_null<Instruction>(edge->getDst()->getValue());
    if (producer == nullptr || consumer == nullptr || producer != consumer) {
      continue;
    }

    auto *call = dyn_cast<CallInst>(producer);
    if (!isThreadSafeLibraryCall(call)) {
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
  if (this->dependenceGraph == nullptr ||
      this->memoryCloningAnalysis == nullptr) {
    return;
  }

  std::vector<LoopDependenceEdge *> edgesToRemove;
  std::vector<std::pair<Value *, Value *>> replacementEdges;
  for (auto *edge : this->dependenceGraph->getEdges()) {
    if (!edge->isLoopCarried() ||
        edge->getKind() != LoopDependenceEdgeKind::Memory) {
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
        if (locationProducer->getAllocation() !=
            locationConsumer->getAllocation()) {
          continue;
        }
        bool producerStores =
            locationProducer->isInstructionStoringLocation(producer);
        bool consumerStores =
            locationConsumer->isInstructionStoringLocation(consumer);
        bool producerLoads =
            locationProducer->isInstructionLoadingLocation(producer);
        bool consumerLoads =
            locationConsumer->isInstructionLoadingLocation(consumer);

        bool isRAW = producerStores && consumerLoads;
        bool isWAR = producerLoads && consumerStores;
        bool isWAW = producerStores && consumerStores;
        if (isRAW || isWAR || isWAW) {
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

    replacementEdges.emplace_back(producer, consumer);
    edge->setLoopCarried(false);
    edgesToRemove.push_back(edge);
  }

  for (auto &pair : replacementEdges) {
    this->dependenceGraph->addVariableDependence(pair.first, pair.second, true);
  }
  for (auto *edge : edgesToRemove) {
    this->dependenceGraph->removeEdge(edge);
  }
}

void LoopContent::materializeScalarAnalyses(
    llvm::ScalarEvolution &SE, llvm::LoopInfo &LI,
    const noelle::DominatorSummary &DS) {
  assert(this->dependenceGraph != nullptr);
  assert(this->sccdag != nullptr);

  this->invalidateDerivedAnalyses();

  LoopCarriedDependencies::setLoopCarriedDependencies(this->loop, DS,
                                                      *this->dependenceGraph);
  {
    std::vector<std::unique_ptr<LoopDependenceRefinementPass>> refinementPasses;
    refinementPasses.push_back(
        std::unique_ptr<LoopDependenceRefinementPass>(
            new UnsupportedLoopAwareDependenceRefinementPass()));
    refinementPasses.push_back(
        std::unique_ptr<LoopDependenceRefinementPass>(
            new AffineLoopDependenceRefinementPass()));
    for (auto &pass : refinementPasses) {
      pass->refine(*this, SE, LI, DS);
    }
  }

  this->memoryCloningAnalysis.reset(new MemoryCloningAnalysis(
      this->getLoopStructure(), const_cast<noelle::DominatorSummary &>(DS),
      this->dependenceGraph.get()));

  this->removeMemoryCloningNegatedDependences();
  LoopCarriedDependencies::setLoopCarriedDependencies(this->loop, DS,
                                                      *this->dependenceGraph);
  this->removeThreadSafeLibraryDependences();

  this->rebuildSCCDAG();
  LoopCarriedDependencies::setLoopCarriedDependencies(this->loop, DS,
                                                      *this->dependenceGraph);

  std::set<Value *> excludeValues;
  for (auto *memoryObject :
       this->memoryCloningAnalysis->getClonableMemoryObjects()) {
    if (!memoryObject->doPrivateCopiesNeedToBeInitialized()) {
      excludeValues.insert(memoryObject->getAllocation());
    }
  }
  this->buildEnvironment(excludeValues);

  this->invariantManager.reset(new InvariantManager(
      this->getLoopStructure(), this->dependenceGraph.get()));
  this->ivDependenceGraph =
      this->dependenceGraph->createSubgraph(/*includeControl=*/true,
                                            /*includeVariable=*/true,
                                            /*includeMemory=*/false);
  this->ivSCCDAG.reset(new LoopSCCDAG(*this->ivDependenceGraph));
  this->inductionVariables.reset(
      new InductionVariableManager(this->loop, *this->invariantManager, SE, LI,
                                   *this->ivSCCDAG, *this->environment));
}

void LoopContent::materializeIterationSpaceAnalysis(llvm::ScalarEvolution &SE) {
  assert(this->inductionVariables != nullptr);
  this->iterationSpaceAnalysis.reset(new LoopIterationSpaceAnalysis(
      this->loop, *this->inductionVariables, SE));
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
  this->sccAttrs.reset(new SCCDAGAttrs(
      enableFloatAsReal, this->dependenceGraph.get(), this->sccdag.get(),
      this->loop, *this->inductionVariables, DS));
}

} // namespace loop
} // namespace analysis
} // namespace lotus
