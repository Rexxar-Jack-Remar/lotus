/*
 * Copyright 2026  Lotus contributors
 */
#include "Analysis/Loop/InductionVariable.h"

namespace lotus {
namespace analysis {
namespace loop {

InductionVariable::InductionVariable(
    LoopStructure *loop,
    PHINode *loopEntryPHI,
    Value *startValue,
    const llvm::SCEV *stepSCEV,
    Value *singleStepValue,
    std::unordered_set<Instruction *> instructions)
    : loop{loop},
      loopEntryPHI{loopEntryPHI},
      startValue{startValue},
      stepSCEV{stepSCEV},
      singleStepValue{singleStepValue},
      instructions{std::move(instructions)} {}

PHINode *InductionVariable::getLoopEntryPHI(void) const {
  return this->loopEntryPHI;
}

std::unordered_set<Instruction *> InductionVariable::getAllInstructions(void) const {
  return this->instructions;
}

Value *InductionVariable::getStartValue(void) const { return this->startValue; }

Value *InductionVariable::getSingleComputedStepValue(void) const {
  return this->singleStepValue;
}

const llvm::SCEV *InductionVariable::getStepSCEV(void) const {
  return this->stepSCEV;
}

Type *InductionVariable::getType(void) const {
  return this->loopEntryPHI->getType();
}

bool InductionVariable::isIVInstruction(Instruction *I) const {
  return this->instructions.count(I) != 0;
}

} // namespace loop
} // namespace analysis
} // namespace lotus
