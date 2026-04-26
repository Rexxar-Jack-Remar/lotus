/*
 * Copyright 2026 Lotus contributors
 */
#include "Analysis/Loop/LoopContent.h"

namespace lotus {
namespace analysis {
namespace loop {

namespace {

} // namespace

LoopContent::LoopContent(LoopTree *loopNode) : loop{loopNode} {}

void LoopContent::invalidateAnalysesAfterDependenceGraph(void) {
  this->invalidateAnalysesAfterScalarAnalyses();
}

void LoopContent::invalidateAnalysesAfterScalarAnalyses(void) {
  this->ivDependenceGraph.reset();
  this->ivSCCDAG.reset();
  this->environment.reset();
  this->invariantManager.reset();
  this->inductionVariables.reset();
  this->iterationSpaceAnalysis.reset();
  this->sccAttrs.reset();
}

void LoopContent::materializeDependenceGraph(pdg::ProgramGraph &pdg) {
  auto bundle = LoopLDGBuilder::buildBaseLoopDependenceGraph(this->loop, pdg);
  this->dependenceGraph = std::move(bundle.graph);
  this->sccdag = std::move(bundle.sccdag);
  this->dependenceGraphDebugDumps = std::move(bundle.debugSnapshots);
  this->memoryCloningAnalysis.reset();
  this->invalidateAnalysesAfterDependenceGraph();
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
  this->materializeEnvironmentFromMemoryCloning();
}

void LoopContent::materializeLoopCarriedDependencies(
    const noelle::DominatorSummary &DS) {
  assert(this->dependenceGraph != nullptr);
  LoopCarriedDependencies::setLoopCarriedDependencies(this->loop, DS,
                                                      *this->dependenceGraph);
}

void LoopContent::materializeEnvironmentFromMemoryCloning(void) {
  if (this->environment != nullptr) {
    return;
  }

  std::set<Value *> excludeValues;
  if (this->memoryCloningAnalysis != nullptr) {
    for (auto *memoryObject :
         this->memoryCloningAnalysis->getClonableMemoryObjects()) {
      if (memoryObject == nullptr ||
          memoryObject->doPrivateCopiesNeedToBeInitialized()) {
        continue;
      }
      excludeValues.insert(memoryObject->getAllocation());
    }
  }

  this->buildEnvironment(excludeValues);
}

void LoopContent::materializeInductionVariables(
    llvm::ScalarEvolution &SE, llvm::LoopInfo &LI,
    LoopLDGBuilderOptions const &options) {
  auto loopInternalGraph =
      LoopLDGBuilder::createInternalSubgraph(*this->dependenceGraph);
  assert(loopInternalGraph != nullptr);
  this->ivDependenceGraph =
      loopInternalGraph->createSubgraph(/*includeControl=*/true,
                                        /*includeVariable=*/true,
                                        /*includeMemory=*/false);
  assert(this->ivDependenceGraph != nullptr);
  this->ivSCCDAG.reset(new LoopSCCDAG(*this->ivDependenceGraph));

  this->inductionVariables.reset(new InductionVariableManager(
      this->loop, *this->invariantManager, SE, LI, *this->ivSCCDAG,
      *this->environment, options.enableExtendedIVRecognition));
}

void LoopContent::materializeScalarAnalyses(
    llvm::ScalarEvolution &SE, llvm::LoopInfo &LI,
    const noelle::DominatorSummary &DS,
    LoopLDGBuilderOptions options) {
  assert(this->dependenceGraph != nullptr);

  auto refined = LoopLDGBuilder::refineLoopDependenceGraph(
      std::move(this->dependenceGraph), SE, LI, DS, options);
  this->dependenceGraph = std::move(refined.graph);
  this->sccdag = std::move(refined.sccdag);
  if (!this->dependenceGraphDebugDumps.empty()) {
    auto existingBaseSnapshots = std::move(this->dependenceGraphDebugDumps);
    this->dependenceGraphDebugDumps = std::move(existingBaseSnapshots);
    this->dependenceGraphDebugDumps.insert(
        this->dependenceGraphDebugDumps.end(),
        std::make_move_iterator(refined.debugSnapshots.begin()),
        std::make_move_iterator(refined.debugSnapshots.end()));
  } else {
    this->dependenceGraphDebugDumps = std::move(refined.debugSnapshots);
  }

  this->memoryCloningAnalysis.reset(new MemoryCloningAnalysis(
      this->getLoopStructure(), const_cast<noelle::DominatorSummary &>(DS),
      this->dependenceGraph.get()));
  this->invalidateAnalysesAfterScalarAnalyses();
  this->materializeEnvironmentFromMemoryCloning();
  this->invariantManager.reset(new InvariantManager(
      this->getLoopStructure(), this->dependenceGraph.get()));
  this->materializeInductionVariables(SE, LI, options);
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
