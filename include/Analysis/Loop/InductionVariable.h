/** @file InductionVariable.h @brief Individual induction variable descriptor for loop analysis. */
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
#ifndef LOTUS_ANALYSIS_LOOP_INDUCTIONVARIABLE_H
#define LOTUS_ANALYSIS_LOOP_INDUCTIONVARIABLE_H

#include "Analysis/Loop/LoopStructure.h"
#include "Analysis/Loop/Invariants.h"
#include "Analysis/Loop/LoopEnvironment.h"
#include "Analysis/Loop/LoopSCCDAG.h"
#include "Analysis/Loop/ScalarEvolutionReferencer.h"

#include <memory>

#include "llvm/Analysis/ScalarEvolution.h"

namespace lotus {
namespace analysis {
namespace loop {

class InductionVariable {
public:
  InductionVariable(LoopStructure *loop,
                    LoopSCC *scc,
                    PHINode *loopEntryPHI,
                    Value *startValue,
                    const llvm::SCEV *stepSCEV,
                    Value *singleStepValue,
                    std::unordered_set<PHINode *> stepPHIs,
                    std::unordered_set<PHINode *> phis,
                    std::unordered_set<Instruction *> nonPHIInstructions,
                    std::unordered_set<Instruction *> instructions,
                    std::unordered_set<Instruction *> derivedInstructions);

  InductionVariable(LoopStructure *loop,
                    InvariantManager &IVM,
                    llvm::ScalarEvolution &SE,
                    int64_t stepMultiplier,
                    PHINode *loopEntryPHI,
                    std::unordered_set<PHINode *> stepPHIs,
                    LoopSCC *scc,
                    LoopEnvironment &loopEnvironment,
                    llvm::ScalarEvolutionReferentialExpander &referentialExpander);

  InductionVariable(LoopStructure *loop,
                    InvariantManager &IVM,
                    llvm::ScalarEvolution &SE,
                    PHINode *loopEntryPHI,
                    LoopSCC *scc,
                    LoopEnvironment &loopEnvironment,
                    llvm::ScalarEvolutionReferentialExpander &referentialExpander,
                    llvm::InductionDescriptor &descriptor);

  LoopSCC *getSCC(void) const;
  PHINode *getLoopEntryPHI(void) const;
  std::unordered_set<PHINode *> getPHIsInvolvedInComputingIVStep(void) const;
  std::unordered_set<PHINode *> getPHIs(void) const;
  std::unordered_set<Instruction *> getNonPHIIntermediateValues(void) const;
  std::unordered_set<Instruction *> getAllInstructions(void) const;
  std::unordered_set<Instruction *> getDerivedSCEVInstructions(void) const;
  Value *getStartValue(void) const;
  Value *getSingleComputedStepValue(void) const;
  std::vector<Instruction *> getComputationOfStepValue(void) const;
  bool isStepValueLoopInvariant(void) const;
  bool isStepValuePositive(void) const;
  const llvm::SCEV *getStepSCEV(void) const;
  Type *getType(void) const;
  bool isIVInstruction(Instruction *I) const;
  bool isDerivedFromIVInstructions(Instruction *I) const;

private:
  void collectValuesInternalAndExternalToLoopAndSCC(
      LoopEnvironment &loopEnvironment);
  void deriveStepValue(llvm::ScalarEvolution &SE,
                       llvm::ScalarEvolutionReferentialExpander &referentialExpander,
                       int64_t multiplier);
  void deriveStepValueFromSCEVConstant(const llvm::SCEVConstant *scev,
                                       int64_t multiplier);
  void deriveStepValueFromSCEVUnknown(const llvm::SCEVUnknown *scev);
  bool deriveStepValueFromCompositeSCEV(
      const llvm::SCEV *scev,
      llvm::ScalarEvolutionReferentialExpander &referentialExpander);
  void traverseCycleThroughLoopEntryPHIToGetAllIVInstructions(void);
  void traverseConsumersOfIVInstructionsToGetAllDerivedSCEVInstructions(
      InvariantManager &IVM,
      llvm::ScalarEvolution &SE);

  LoopStructure *loop;
  LoopSCC *scc;
  PHINode *loopEntryPHI;
  std::unordered_set<PHINode *> stepPHIs;
  std::unordered_set<PHINode *> phis;
  std::unordered_set<Instruction *> nonPHIInstructions;
  Value *startValue;
  const llvm::SCEV *stepSCEV;
  Value *singleStepValue;
  int64_t stepMultiplier;
  std::vector<Instruction *> computationOfStepValue;
  bool isComputedStepValueLoopInvariant;
  Type *loopEntryPHIType;
  std::set<Value *> valuesToReferenceInComputingStepValue;
  std::set<Value *> valuesInScopeOfInductionVariable;
  std::unordered_set<Instruction *> instructions;
  std::unordered_set<Instruction *> derivedInstructions;
  std::unique_ptr<BasicBlock> stepComputationScratchBlock;
};

} // namespace loop
} // namespace analysis
} // namespace lotus

#endif
