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
    const llvm::SCEVAddRecExpr *recurrence;
    int64_t constantStep;
    bool isAnalyzed;
  };

  LoopTree *loops;
  InductionVariableManager &ivManager;
  std::unordered_map<Instruction *, std::unique_ptr<MemoryAccessSpace>>
      accessSpaceByInstruction;

  void computeMemoryAccessSpace(llvm::ScalarEvolution &SE);
};

} // namespace loop
} // namespace analysis
} // namespace lotus

#endif
