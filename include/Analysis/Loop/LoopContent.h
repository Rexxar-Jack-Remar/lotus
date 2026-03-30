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
#include "Analysis/Loop/LoopLDGBuilder.h"
#include "Analysis/Loop/LoopSCCDAG.h"
#include "Analysis/Loop/MemoryCloningAnalysis.h"
#include "Analysis/Loop/SCCDAGAttrs.h"

namespace lotus {
namespace analysis {
namespace loop {

class LoopContent {
public:
  explicit LoopContent(LoopTree *loopNode);

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

  void materializeDependenceGraph(pdg::ProgramGraph &pdg);
  const std::vector<std::pair<std::string, std::string>> &
  getDependenceGraphDebugDumps(void) const {
    return this->dependenceGraphDebugDumps;
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
                                 llvm::LoopInfo &LI,
                                 const noelle::DominatorSummary &DS);

  bool hasEnvironment(void) const { return this->environment != nullptr; }

  LoopEnvironment *getEnvironment(void) const {
    assert(this->environment != nullptr);
    return this->environment.get();
  }

  void materializeEnvironment(void);

  void materializeLoopCarriedDependencies(const noelle::DominatorSummary &DS);

  bool hasLoopIterationSpaceAnalysis(void) const {
    return this->iterationSpaceAnalysis != nullptr;
  }

  LoopIterationSpaceAnalysis *getLoopIterationSpaceAnalysis(void) const {
    assert(this->iterationSpaceAnalysis != nullptr);
    return this->iterationSpaceAnalysis.get();
  }

  void materializeIterationSpaceAnalysis(llvm::ScalarEvolution &SE);

  bool hasSCCAttrs(void) const { return this->sccAttrs != nullptr; }

  SCCDAGAttrs *getSCCAttrs(void) const {
    assert(this->sccAttrs != nullptr);
    return this->sccAttrs.get();
  }

  void materializeSCCAttrs(bool enableFloatAsReal,
                           noelle::DominatorSummary &DS);

  bool doesHaveCompileTimeKnownTripCount(void) const {
    return this->compileTimeKnownTripCount;
  }

  uint64_t getCompileTimeTripCount(void) const { return this->tripCount; }

  void setCompileTimeTripCount(uint64_t tripCount) {
    this->compileTimeKnownTripCount = (tripCount > 0);
    this->tripCount = tripCount;
  }

private:
  void invalidateDerivedAnalyses(void);
  void rebuildSCCDAG(void);
  void buildEnvironment(const std::set<Value *> &excludeValues);
  void removeMemoryCloningNegatedDependences(void);
  void removeThreadSafeLibraryDependences(void);
  void removeLoopCarriedDependencesProvedDisjoint(
      LoopIterationSpaceAnalysis &analysis);
  bool canInstructionReachWithinSameIteration(Instruction *from,
                                              Instruction *to) const;

  LoopTree *loop;
  std::unique_ptr<LoopDependenceGraph> dependenceGraph;
  std::unique_ptr<LoopSCCDAG> sccdag;
  std::vector<std::pair<std::string, std::string>> dependenceGraphDebugDumps;
  std::unique_ptr<LoopDependenceGraph> ivDependenceGraph;
  std::unique_ptr<LoopSCCDAG> ivSCCDAG;
  std::unique_ptr<LoopEnvironment> environment;
  std::unique_ptr<InvariantManager> invariantManager;
  std::unique_ptr<InductionVariableManager> inductionVariables;
  std::unique_ptr<LoopIterationSpaceAnalysis> iterationSpaceAnalysis;
  std::unique_ptr<MemoryCloningAnalysis> memoryCloningAnalysis;
  std::unique_ptr<SCCDAGAttrs> sccAttrs;
  bool compileTimeKnownTripCount{false};
  uint64_t tripCount{0};
};

} // namespace loop
} // namespace analysis
} // namespace lotus

#endif
