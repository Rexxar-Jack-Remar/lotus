/*
 * Copyright 2026  Lotus contributors
 */
#include "Analysis/Loop/InductionVariable.h"

namespace lotus {
namespace analysis {
namespace loop {

InductionVariable::InductionVariable(
    LoopStructure *loop,
    LoopSCC *scc,
    PHINode *loopEntryPHI,
    Value *startValue,
    const llvm::SCEV *stepSCEV,
    Value *singleStepValue,
    std::unordered_set<PHINode *> stepPHIs,
    std::unordered_set<PHINode *> phis,
    std::unordered_set<Instruction *> nonPHIInstructions,
    std::unordered_set<Instruction *> instructions,
    std::unordered_set<Instruction *> derivedInstructions)
    : loop{loop},
      scc{scc},
      loopEntryPHI{loopEntryPHI},
      stepPHIs{std::move(stepPHIs)},
      phis{std::move(phis)},
      nonPHIInstructions{std::move(nonPHIInstructions)},
      startValue{startValue},
      stepSCEV{stepSCEV},
      singleStepValue{singleStepValue},
      instructions{std::move(instructions)},
      derivedInstructions{std::move(derivedInstructions)} {}

LoopSCC *InductionVariable::getSCC(void) const { return this->scc; }

PHINode *InductionVariable::getLoopEntryPHI(void) const {
  return this->loopEntryPHI;
}

std::unordered_set<PHINode *> InductionVariable::getPHIsInvolvedInComputingIVStep(
    void) const {
  return this->stepPHIs;
}

std::unordered_set<PHINode *> InductionVariable::getPHIs(void) const {
  return this->phis;
}

std::unordered_set<Instruction *> InductionVariable::getNonPHIIntermediateValues(
    void) const {
  return this->nonPHIInstructions;
}

std::unordered_set<Instruction *> InductionVariable::getAllInstructions(void) const {
  return this->instructions;
}

std::unordered_set<Instruction *> InductionVariable::getDerivedSCEVInstructions(
    void) const {
  return this->derivedInstructions;
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

bool InductionVariable::isDerivedFromIVInstructions(Instruction *I) const {
  return this->derivedInstructions.count(I) != 0;
}

} // namespace loop
} // namespace analysis
} // namespace lotus
