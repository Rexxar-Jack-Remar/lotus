/** @file LoopIterationSpaceAnalysis.h @brief Analysis of loop iteration spaces for parallelization and optimization. */
/*
 * Copyright 2026 Lotus contributors
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
#ifndef LOTUS_ANALYSIS_LOOP_LOOPITERATIONSPACEANALYSIS_H
#define LOTUS_ANALYSIS_LOOP_LOOPITERATIONSPACEANALYSIS_H

#include "Analysis/Loop/InductionVariables.h"
#include "Analysis/Loop/LoopDependenceGraph.h"

namespace lotus {
namespace analysis {
namespace loop {

class LoopIterationSpaceAnalysis {
public:
  LoopIterationSpaceAnalysis(LoopTree *loops,
                             InductionVariableManager &ivManager,
                             llvm::ScalarEvolution &SE);

  bool areInstructionsAccessingDisjointMemoryLocationsBetweenIterations(
      Instruction *from,
      Instruction *to) const;

private:
  struct MemoryAccessSpace {
    explicit MemoryAccessSpace(Instruction *memoryAccessor);

    Instruction *memoryAccessor;
    const llvm::SCEV *memoryAccessorSCEV;
    const llvm::SCEV *memoryAccessorBasePointerSCEV;
    const llvm::SCEV *memoryMinusSCEV;
    const llvm::SCEVAddRecExpr *recurrence;
    const llvm::SCEV *elementSize;
    std::vector<const llvm::SCEV *> subscripts;
    std::vector<const llvm::SCEV *> sizes;
    std::vector<std::pair<Instruction *, InductionVariable *>> subscriptIVs;
    std::set<Instruction *> accessInstructions;
    int64_t constantStep;
    bool isAnalyzed;
  };

  LoopTree *loops;
  InductionVariableManager &ivManager;
  std::unordered_map<const llvm::SCEV *, std::unordered_set<Instruction *>>
      ivInstructionsBySCEV;
  std::unordered_map<const llvm::SCEV *, std::unordered_set<Instruction *>>
      derivedInstructionsFromIVsBySCEV;
  std::unordered_map<Instruction *, InductionVariable *> ivsByInstruction;
  std::set<MemoryAccessSpace *> nonOverlappingAccessesBetweenIterations;
  std::unordered_map<MemoryAccessSpace *, std::set<MemoryAccessSpace *>>
      spacesThatCannotOverlap;
  std::vector<std::unique_ptr<MemoryAccessSpace>> accessSpaces;
  std::unordered_map<Instruction *, MemoryAccessSpace *> accessSpaceByInstruction;

  void indexIVInstructionSCEVs(llvm::ScalarEvolution &SE);
  void computeMemoryAccessSpace(llvm::ScalarEvolution &SE);
  void identifyIVForMemoryAccessSubscripts(llvm::ScalarEvolution &SE);
  void identifyNonOverlappingAccessesBetweenIterationsAcrossOneLoopInvocation(
      llvm::ScalarEvolution &SE);
  bool areMemoryAccessSpaceNotOverlappingOrExactlyTheSame(
      MemoryAccessSpace *space1,
      MemoryAccessSpace *space2) const;
  bool analyzeToCheckIfMemoryAccessSpaceNotOverlappingOrExactlyTheSame(
      MemoryAccessSpace *space1,
      MemoryAccessSpace *space2) const;
  bool isMemoryAccessSpaceEquivalentForTopLoopIVSubscript(
      MemoryAccessSpace *space1,
      MemoryAccessSpace *space2) const;
  bool isOneToOneFunctionOnIV(llvm::ScalarEvolution &SE,
                              LoopStructure *loopStructure,
                              InductionVariable *IV,
                              Instruction *derivedInstruction) const;
  bool isInnerDimensionSubscriptsBounded(llvm::ScalarEvolution &SE,
                                         MemoryAccessSpace *space);
};

} // namespace loop
} // namespace analysis
} // namespace lotus

#endif
