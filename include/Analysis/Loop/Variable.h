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
#ifndef LOTUS_ANALYSIS_LOOP_VARIABLE_H
#define LOTUS_ANALYSIS_LOOP_VARIABLE_H

#include "Analysis/Loop/LoopCarriedDependencies.h"

#include <set>
#include <unordered_set>

namespace lotus {
namespace analysis {
namespace loop {

class EvolutionUpdate {
public:
  EvolutionUpdate(Instruction *updateInstruction,
                  const std::unordered_set<Value *> &internalVariableValues);

  bool mayUpdateBeOverride(void) const;
  bool isCommutativeWithSelf(void) const;
  bool isAssociativeWithSelf(void) const;
  bool isTransformablyCommutativeWithSelf(void) const;
  bool isTransformablyCommutativeWith(const EvolutionUpdate &other) const;
  bool isAssociativeWith(const EvolutionUpdate &other) const;
  Instruction *getUpdateInstruction(void) const;
  bool isAdd(void) const;
  bool isMul(void) const;
  bool isSub(void) const;
  bool isSubTransformableToAdd(void) const;

private:
  bool isBothUpdatesAddOrSub(const EvolutionUpdate &other) const;
  bool isBothUpdatesMul(const EvolutionUpdate &other) const;
  bool isBothUpdatesSameBitwiseLogicalOp(const EvolutionUpdate &other) const;

  Instruction *updateInstruction;
  Value *newValue;
  std::unordered_set<Use *> internalValuesUsed;
  std::unordered_set<Use *> externalValuesUsed;
};

class LoopCarriedVariable {
public:
  LoopCarriedVariable(const LoopStructure &loop, LoopTree *loopNode,
                      LoopDependenceGraph &loopDG, LoopSCCDAG &sccdag,
                      LoopSCC &variableSCC, PHINode *declarationPHI);

  ~LoopCarriedVariable();

  bool isEvolutionReducibleAcrossLoopIterations(void) const;
  PHINode *getLoopEntryPHIForValueOfVariable(Value *value) const;
  Value *getInitialValue(void) const;
  Instruction::BinaryOps getReductionOperation(void) const;
  PHINode *getAccumulator(void) const;
  Value *getIdentityValue(void) const;

private:
  std::unordered_set<Value *>
  produceDataAndMemoryOnlySCC(LoopDependenceGraph &loopDG,
                              const std::unordered_set<LoopDependenceEdge *>
                                  &loopCarriedDependenciesNotOfVariable) const;
  void collectControlValuesGoverningEvolution(
      LoopDependenceGraph &loopDG,
      const std::unordered_set<LoopDependenceEdge *>
          &loopCarriedDependenciesNotOfVariable);
  bool
  collectVariableUpdates(const std::unordered_set<Value *> &loopCarriedValues);
  std::unordered_set<Value *> getConsumersOfVariable(void) const;
  bool areValuesPropagatingVariableIntermediatesOutsideLoop(
      const std::unordered_set<Value *> &values) const;
  bool hasRoundingError(
      std::unordered_set<EvolutionUpdate *> &arithmeticUpdates) const;

  bool isValid;
  const LoopStructure &outermostLoopOfVariable;
  PHINode *declarationPHI;
  std::unordered_set<Value *> sccOfVariableOnlyValues;
  std::unordered_set<Value *> sccOfDataAndMemoryVariableValuesOnly;
  std::set<Value *> controlValuesGoverningEvolution;
  std::unordered_set<EvolutionUpdate *> variableUpdates;
  std::unordered_set<EvolutionUpdate *> loopCarriedVariableUpdates;
  std::unordered_set<CastInst *> castsInternalToVariableComputation;
  Value *initialValue;
  mutable PHINode *accumulator;
  mutable Instruction::BinaryOps reductionOperation;
  mutable Value *identityValue;
};

} // namespace loop
} // namespace analysis
} // namespace lotus

#endif
