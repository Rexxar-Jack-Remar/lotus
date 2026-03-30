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

namespace lotus {
namespace analysis {
namespace loop {

class EvolutionUpdate {
public:
  explicit EvolutionUpdate(Instruction *updateInstruction);

  bool mayUpdateBeOverride(void) const;
  bool isTransformablyCommutativeWith(const EvolutionUpdate &other) const;
  bool isAssociativeWith(const EvolutionUpdate &other) const;
  Instruction *getUpdateInstruction(void) const;
  bool isAdd(void) const;
  bool isMul(void) const;

private:
  Instruction *updateInstruction;
};

class LoopCarriedVariable {
public:
  LoopCarriedVariable(const LoopStructure &loop,
                      LoopTree *loopNode,
                      LoopDependenceGraph &loopDG,
                      LoopSCCDAG &sccdag,
                      LoopSCC &variableSCC,
                      PHINode *declarationPHI);

  bool isEvolutionReducibleAcrossLoopIterations(void) const;
  PHINode *getLoopEntryPHIForValueOfVariable(Value *value) const;
  Value *getInitialValue(void) const;
  Instruction::BinaryOps getReductionOperation(void) const;
  PHINode *getAccumulator(void) const;
  Value *getIdentityValue(void) const;

private:
  bool isValid;
  const LoopStructure &outermostLoopOfVariable;
  PHINode *declarationPHI;
  Value *initialValue;
  PHINode *accumulator;
  Instruction::BinaryOps reductionOperation;
  Value *identityValue;
};

} // namespace loop
} // namespace analysis
} // namespace lotus

#endif
