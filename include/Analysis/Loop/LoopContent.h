/*
 * Copyright 2016 - 2025  Angelo Matni, Simone Campanoni, Lotus contributors
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE
 * OR OTHER DEALINGS IN THE SOFTWARE.
 */
#ifndef LOTUS_ANALYSIS_LOOP_LOOPCONTENT_H
#define LOTUS_ANALYSIS_LOOP_LOOPCONTENT_H

#include "Analysis/Loop/InductionVariables.h"
#include "Analysis/Loop/Invariants.h"
#include "Analysis/Loop/LoopCarriedDependencies.h"
#include "Analysis/Loop/LoopDependenceGraph.h"
#include "Analysis/Loop/LoopEnvironment.h"
#include "Analysis/Loop/LoopForest.h"
#include "Analysis/Loop/LoopIterationSpaceAnalysis.h"
#include "Analysis/Loop/LoopSCCDAG.h"
#include "Analysis/Loop/SCCDAGAttrs.h"

namespace lotus {
namespace analysis {
namespace loop {

class LoopContent {
public:
  explicit LoopContent(LoopTree *loopNode) : loop{loopNode} {}

  LoopTree *getLoopHierarchyStructures(void) const { return this->loop; }

  LoopStructure *getLoopStructure(void) const {
    return this->loop ? this->loop->getLoop() : nullptr;
  }

  LoopStructure *getNestedMostLoopStructure(Instruction *I) const {
    return this->loop ? this->loop->getInnermostLoopThatContains(I) : nullptr;
  }

  bool hasDependenceGraph(void) const { return this->dependenceGraph != nullptr; }

  LoopDependenceGraph *getLoopDependenceGraph(void) const {
    assert(this->dependenceGraph != nullptr);
    return this->dependenceGraph.get();
  }

  bool hasSCCDAG(void) const { return this->sccdag != nullptr; }

  LoopSCCDAG *getSCCDAG(void) const {
    assert(this->sccdag != nullptr);
    return this->sccdag.get();
  }

  void materializeDependenceGraph(pdg::ProgramGraph &pdg) {
    this->dependenceGraph.reset(new LoopDependenceGraph(this->loop, pdg));
    this->sccdag.reset(new LoopSCCDAG(*this->dependenceGraph));
  }

  bool hasInvariantManager(void) const {
    return this->invariantManager != nullptr;
  }

  InvariantManager *getInvariantManager(void) const {
    assert(this->invariantManager != nullptr);
    return this->invariantManager.get();
  }

  bool hasInductionVariableManager(void) const {
    return this->inductionVariables != nullptr;
  }

  InductionVariableManager *getInductionVariableManager(void) const {
    assert(this->inductionVariables != nullptr);
    return this->inductionVariables.get();
  }

  void materializeScalarAnalyses(llvm::ScalarEvolution &SE,
                                 llvm::LoopInfo &LI) {
    assert(this->dependenceGraph != nullptr);
    assert(this->sccdag != nullptr);
    this->invariantManager.reset(
        new InvariantManager(this->getLoopStructure(), this->dependenceGraph.get()));
    this->inductionVariables.reset(new InductionVariableManager(
        this->loop, *this->invariantManager, SE, LI, *this->sccdag));
  }

  bool hasEnvironment(void) const { return this->environment != nullptr; }

  LoopEnvironment *getEnvironment(void) const {
    assert(this->environment != nullptr);
    return this->environment.get();
  }

  void materializeEnvironment(void) {
    assert(this->dependenceGraph != nullptr);
    this->environment.reset(new LoopEnvironment(
        this->dependenceGraph.get(), this->getLoopStructure()->getLoopExitBasicBlocks()));
  }

  void materializeLoopCarriedDependencies(const noelle::DominatorSummary &DS) {
    assert(this->dependenceGraph != nullptr);
    LoopCarriedDependencies::setLoopCarriedDependencies(
        this->loop, DS, *this->dependenceGraph);
  }

  bool hasLoopIterationSpaceAnalysis(void) const {
    return this->iterationSpaceAnalysis != nullptr;
  }

  LoopIterationSpaceAnalysis *getLoopIterationSpaceAnalysis(void) const {
    assert(this->iterationSpaceAnalysis != nullptr);
    return this->iterationSpaceAnalysis.get();
  }

  void materializeIterationSpaceAnalysis(llvm::ScalarEvolution &SE) {
    assert(this->inductionVariables != nullptr);
    this->iterationSpaceAnalysis.reset(
        new LoopIterationSpaceAnalysis(this->loop, *this->inductionVariables, SE));

    if (this->dependenceGraph == nullptr) {
      return;
    }
    for (auto *edge : this->dependenceGraph->getEdges()) {
      if (!edge->isLoopCarried() || edge->getKind() != LoopDependenceEdgeKind::Memory) {
        continue;
      }
      auto *src = dyn_cast_or_null<Instruction>(edge->getSrc()->getValue());
      auto *dst = dyn_cast_or_null<Instruction>(edge->getDst()->getValue());
      if (src == nullptr || dst == nullptr) {
        continue;
      }
      if (this->iterationSpaceAnalysis
              ->areInstructionsAccessingDisjointMemoryLocationsBetweenIterations(
                  src, dst)) {
        edge->setLoopCarried(false);
      }
    }
  }

  bool hasSCCAttrs(void) const { return this->sccAttrs != nullptr; }

  SCCDAGAttrs *getSCCAttrs(void) const {
    assert(this->sccAttrs != nullptr);
    return this->sccAttrs.get();
  }

  void materializeSCCAttrs(bool enableFloatAsReal,
                           noelle::DominatorSummary &DS) {
    assert(this->dependenceGraph != nullptr);
    assert(this->sccdag != nullptr);
    assert(this->inductionVariables != nullptr);
    this->sccAttrs.reset(new SCCDAGAttrs(enableFloatAsReal,
                                         this->dependenceGraph.get(),
                                         this->sccdag.get(),
                                         this->loop,
                                         *this->inductionVariables,
                                         DS));
  }

  bool doesHaveCompileTimeKnownTripCount(void) const {
    return this->compileTimeKnownTripCount;
  }

  uint64_t getCompileTimeTripCount(void) const { return this->tripCount; }

  void setCompileTimeTripCount(uint64_t tripCount) {
    this->compileTimeKnownTripCount = (tripCount > 0);
    this->tripCount = tripCount;
  }

private:
  LoopTree *loop;
  std::unique_ptr<LoopDependenceGraph> dependenceGraph;
  std::unique_ptr<LoopSCCDAG> sccdag;
  std::unique_ptr<LoopEnvironment> environment;
  std::unique_ptr<InvariantManager> invariantManager;
  std::unique_ptr<InductionVariableManager> inductionVariables;
  std::unique_ptr<LoopIterationSpaceAnalysis> iterationSpaceAnalysis;
  std::unique_ptr<SCCDAGAttrs> sccAttrs;
  bool compileTimeKnownTripCount{false};
  uint64_t tripCount{0};
};

} // namespace loop
} // namespace analysis
} // namespace lotus

#endif
