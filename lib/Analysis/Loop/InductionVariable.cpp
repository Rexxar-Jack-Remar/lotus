/*
 * Copyright 2026 Lotus contributors
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
      stepMultiplier{1},
      computationOfStepValue{},
      isComputedStepValueLoopInvariant{
          singleStepValue != nullptr && loop != nullptr && loop->isLoopInvariant(singleStepValue)},
      loopEntryPHIType{loopEntryPHI ? loopEntryPHI->getType() : nullptr},
      valuesToReferenceInComputingStepValue{},
      valuesInScopeOfInductionVariable{},
      instructions{std::move(instructions)},
      derivedInstructions{std::move(derivedInstructions)} {}

InductionVariable::InductionVariable(
    LoopStructure *LS,
    InvariantManager &IVM,
    llvm::ScalarEvolution &SE,
    PHINode *loopEntryPHI,
    LoopSCC *scc,
    LoopEnvironment &loopEnvironment,
    llvm::ScalarEvolutionReferentialExpander &referentialExpander,
    llvm::InductionDescriptor &descriptor)
    : loop{LS},
      scc{scc},
      loopEntryPHI{loopEntryPHI},
      stepPHIs{std::unordered_set<PHINode *>({loopEntryPHI})},
      phis{},
      nonPHIInstructions{},
      startValue{descriptor.getStartValue()},
      stepSCEV{descriptor.getStep()},
      singleStepValue{descriptor.getConstIntStepValue()},
      stepMultiplier{1},
      computationOfStepValue{},
      isComputedStepValueLoopInvariant{false},
      loopEntryPHIType{loopEntryPHI->getType()},
      valuesToReferenceInComputingStepValue{},
      valuesInScopeOfInductionVariable{},
      instructions{},
      derivedInstructions{} {
  this->traverseCycleThroughLoopEntryPHIToGetAllIVInstructions();
  this->traverseConsumersOfIVInstructionsToGetAllDerivedSCEVInstructions(IVM, SE);
  this->collectValuesInternalAndExternalToLoopAndSCC(loopEnvironment);

  if (descriptor.getKind() == llvm::InductionDescriptor::InductionKind::IK_FpInduction) {
    this->singleStepValue = cast<llvm::SCEVUnknown>(this->stepSCEV)->getValue();
    this->isComputedStepValueLoopInvariant = true;
  } else {
    this->deriveStepValue(SE, referentialExpander, this->stepMultiplier);
  }
}

InductionVariable::InductionVariable(
    LoopStructure *LS,
    InvariantManager &IVM,
    llvm::ScalarEvolution &SE,
    int64_t stepMultiplier,
    PHINode *loopEntryPHI,
    std::unordered_set<PHINode *> stepPHIs,
    LoopSCC *scc,
    LoopEnvironment &loopEnvironment,
    llvm::ScalarEvolutionReferentialExpander &referentialExpander)
    : loop{LS},
      scc{scc},
      loopEntryPHI{loopEntryPHI},
      stepPHIs{std::move(stepPHIs)},
      phis{},
      nonPHIInstructions{},
      startValue{nullptr},
      stepSCEV{nullptr},
      singleStepValue{nullptr},
      stepMultiplier{stepMultiplier},
      computationOfStepValue{},
      isComputedStepValueLoopInvariant{false},
      loopEntryPHIType{loopEntryPHI->getType()},
      valuesToReferenceInComputingStepValue{},
      valuesInScopeOfInductionVariable{},
      instructions{},
      derivedInstructions{} {
  auto bbs = LS->getBasicBlocks();
  for (auto i = 0u; i < loopEntryPHI->getNumIncomingValues(); ++i) {
    auto *incomingBB = loopEntryPHI->getIncomingBlock(i);
    if (bbs.find(incomingBB) == bbs.end()) {
      this->startValue = loopEntryPHI->getIncomingValue(i);
      break;
    }
  }

  this->traverseCycleThroughLoopEntryPHIToGetAllIVInstructions();
  this->traverseConsumersOfIVInstructionsToGetAllDerivedSCEVInstructions(IVM, SE);
  this->collectValuesInternalAndExternalToLoopAndSCC(loopEnvironment);
  this->deriveStepValue(SE, referentialExpander, this->stepMultiplier);
}

void InductionVariable::traverseCycleThroughLoopEntryPHIToGetAllIVInstructions(void) {
  if (this->scc == nullptr) {
    return;
  }

  std::queue<LoopDependenceNode *> ivIntermediateValues;
  std::set<Value *> valuesVisited;
  auto *rootNode = this->scc->fetchNode(this->loopEntryPHI);
  if (rootNode == nullptr) {
    return;
  }
  ivIntermediateValues.push(rootNode);

  while (!ivIntermediateValues.empty()) {
    auto *node = ivIntermediateValues.front();
    ivIntermediateValues.pop();

    auto *value = node->getValue();
    if (valuesVisited.count(value) != 0) {
      continue;
    }
    valuesVisited.insert(value);

    auto *instruction = dyn_cast_or_null<Instruction>(value);
    if (instruction == nullptr || !this->loop->isIncluded(instruction)) {
      continue;
    }
    this->instructions.insert(instruction);
    if (auto *phi = dyn_cast<PHINode>(instruction)) {
      this->phis.insert(phi);
    } else {
      this->nonPHIInstructions.insert(instruction);
    }

    for (auto *edge : node->getIncomingEdges()) {
      if (edge->getKind() != LoopDependenceEdgeKind::Variable) {
        continue;
      }
      auto *otherNode = edge->getSrc();
      if (otherNode == nullptr) {
        continue;
      }
      auto *otherValue = otherNode->getValue();
      if (!this->scc->isInternal(otherValue)) {
        continue;
      }
      ivIntermediateValues.push(otherNode);
    }
  }

  std::set<CastInst *> castsToAdd;
  for (auto *intermediateValue : this->instructions) {
    for (auto *user : intermediateValue->users()) {
      auto *castInst = dyn_cast<CastInst>(user);
      if (castInst == nullptr || !this->loop->isIncluded(castInst)) {
        continue;
      }
      castsToAdd.insert(castInst);
    }
  }
  this->instructions.insert(castsToAdd.begin(), castsToAdd.end());
}

void InductionVariable::traverseConsumersOfIVInstructionsToGetAllDerivedSCEVInstructions(
    InvariantManager &IVM,
    llvm::ScalarEvolution &SE) {
  std::unordered_set<Instruction *> checked;
  std::function<bool(Instruction *)> checkIfDerived;
  checkIfDerived = [&](Instruction *I) -> bool {
    if (this->derivedInstructions.count(I) != 0) {
      return true;
    }
    if (checked.count(I) != 0) {
      return false;
    }
    checked.insert(I);

    if (!SE.isSCEVable(I->getType()) || !this->loop->isIncluded(I)) {
      return false;
    }

    auto *scev = SE.getSCEV(I);
    if (!isa<llvm::SCEVCastExpr>(scev) && !isa<llvm::SCEVNAryExpr>(scev)
        && !isa<llvm::SCEVUDivExpr>(scev)) {
      return false;
    }

    bool usesAtLeastOneIVInstruction = false;
    for (auto &use : I->operands()) {
      auto *usedValue = use.get();
      if (isa<ConstantInt>(usedValue) || IVM.isLoopInvariant(usedValue)) {
        continue;
      }

      auto *usedInst = dyn_cast<Instruction>(usedValue);
      if (usedInst == nullptr) {
        return false;
      }
      if (!this->loop->isIncluded(usedInst)) {
        continue;
      }

      auto isIVUse = this->isIVInstruction(usedInst);
      auto isDerivedUse = checkIfDerived(usedInst);
      if (isIVUse || isDerivedUse) {
        usesAtLeastOneIVInstruction = true;
        continue;
      }
      return false;
    }

    if (!usesAtLeastOneIVInstruction) {
      return false;
    }

    this->derivedInstructions.insert(I);
    return true;
  };

  std::queue<Instruction *> intermediates;
  std::unordered_set<Instruction *> visited;
  for (auto *ivInst : this->instructions) {
    intermediates.push(ivInst);
    visited.insert(ivInst);
  }

  while (!intermediates.empty()) {
    auto *I = intermediates.front();
    intermediates.pop();

    for (auto *user : I->users()) {
      auto *userInst = dyn_cast<Instruction>(user);
      if (userInst == nullptr || visited.count(userInst) != 0) {
        continue;
      }
      visited.insert(userInst);
      if (!checkIfDerived(userInst)) {
        continue;
      }
      intermediates.push(userInst);
    }
  }
}

void InductionVariable::collectValuesInternalAndExternalToLoopAndSCC(
    LoopEnvironment &loopEnvironment) {
  if (this->scc != nullptr) {
    for (auto const &internalNodePair : this->scc->internalNodePairs()) {
      this->valuesInScopeOfInductionVariable.insert(internalNodePair.first);
    }
    for (auto const &externalPair : this->scc->externalNodePairs()) {
      this->valuesInScopeOfInductionVariable.insert(externalPair.first);
    }
  }

  for (auto *liveIn : loopEnvironment.getProducers()) {
    this->valuesInScopeOfInductionVariable.insert(liveIn);
    this->valuesToReferenceInComputingStepValue.insert(liveIn);
  }
}

void InductionVariable::deriveStepValue(llvm::ScalarEvolution &SE,
                                        llvm::ScalarEvolutionReferentialExpander &referentialExpander,
                                        int64_t multiplier) {
  if (this->stepSCEV == nullptr) {
    assert(this->stepPHIs.size() == 1 && "Expected one PHI for step value calculation");
    auto *stepSCEVPHI = *this->stepPHIs.begin();
    assert(SE.getSCEV(stepSCEVPHI)->getSCEVType() == llvm::SCEVTypes::scAddRecExpr);
    this->stepSCEV =
        cast<llvm::SCEVAddRecExpr>(SE.getSCEV(stepSCEVPHI))->getStepRecurrence(SE);
  }
  assert(this->stepSCEV != nullptr && "stepSCEV is nullptr");

  switch (this->stepSCEV->getSCEVType()) {
  case llvm::SCEVTypes::scConstant:
    deriveStepValueFromSCEVConstant(cast<llvm::SCEVConstant>(this->stepSCEV), multiplier);
    break;
  case llvm::SCEVTypes::scUnknown:
    deriveStepValueFromSCEVUnknown(cast<llvm::SCEVUnknown>(this->stepSCEV));
    break;
  case llvm::SCEVTypes::scAddExpr:
  case llvm::SCEVTypes::scAddRecExpr:
  case llvm::SCEVTypes::scMulExpr:
  case llvm::SCEVTypes::scSignExtend:
  case llvm::SCEVTypes::scSMaxExpr:
  case llvm::SCEVTypes::scSMinExpr:
  case llvm::SCEVTypes::scTruncate:
  case llvm::SCEVTypes::scUDivExpr:
  case llvm::SCEVTypes::scUMaxExpr:
  case llvm::SCEVTypes::scUMinExpr:
  case llvm::SCEVTypes::scZeroExtend:
    if (!deriveStepValueFromCompositeSCEV(this->stepSCEV, referentialExpander)) {
      this->stepSCEV = nullptr;
    }
    break;
  default:
    this->stepSCEV = nullptr;
    break;
  }
}

void InductionVariable::deriveStepValueFromSCEVConstant(
    const llvm::SCEVConstant *scev,
    int64_t multiplier) {
  if (auto *CI = dyn_cast<ConstantInt>(scev->getValue())) {
    this->singleStepValue = ConstantInt::get(scev->getValue()->getType(),
                                             multiplier * CI->getSExtValue());
  } else {
    this->singleStepValue = scev->getValue();
  }
  this->isComputedStepValueLoopInvariant = true;
}

void InductionVariable::deriveStepValueFromSCEVUnknown(const llvm::SCEVUnknown *scev) {
  this->singleStepValue = scev->getValue();
  this->isComputedStepValueLoopInvariant =
      this->loop->isLoopInvariant(this->singleStepValue);
}

bool InductionVariable::deriveStepValueFromCompositeSCEV(
    const llvm::SCEV *scev,
    llvm::ScalarEvolutionReferentialExpander &referentialExpander) {
  auto *stepSizeReferenceTree =
      referentialExpander.createReferenceTree(scev, this->valuesInScopeOfInductionVariable);
  if (stepSizeReferenceTree == nullptr) {
    return false;
  }

  auto *tempBlock = BasicBlock::Create(this->loopEntryPHI->getContext(),
                                       "temp_basic_block",
                                       this->loop->getFunction());
  IRBuilder<> tempBuilder(tempBlock);
  auto *finalValue = referentialExpander.expandUsingReferenceValues(
      stepSizeReferenceTree,
      this->valuesToReferenceInComputingStepValue,
      tempBuilder);
  if (finalValue == nullptr) {
    delete stepSizeReferenceTree;
    tempBlock->eraseFromParent();
    return false;
  }

  this->isComputedStepValueLoopInvariant = true;
  auto references = stepSizeReferenceTree->collectAllReferences();
  for (auto *reference : references) {
    auto *referenceValue = reference->getValue();
    if (referenceValue == nullptr || !this->loop->isLoopInvariant(referenceValue)) {
      this->isComputedStepValueLoopInvariant = false;
      break;
    }
  }

  if (tempBlock->size() < 2) {
    this->singleStepValue = finalValue;
  }

  for (auto &I : *tempBlock) {
    this->computationOfStepValue.push_back(&I);
  }

  delete stepSizeReferenceTree;
  tempBlock->eraseFromParent();
  return true;
}

LoopSCC *InductionVariable::getSCC(void) const { return this->scc; }
PHINode *InductionVariable::getLoopEntryPHI(void) const { return this->loopEntryPHI; }

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
Value *InductionVariable::getSingleComputedStepValue(void) const { return this->singleStepValue; }
std::vector<Instruction *> InductionVariable::getComputationOfStepValue(void) const {
  return this->computationOfStepValue;
}
bool InductionVariable::isStepValueLoopInvariant(void) const {
  return this->isComputedStepValueLoopInvariant;
}

bool InductionVariable::isStepValuePositive(void) const {
  assert(this->isComputedStepValueLoopInvariant);
  auto *stepValue = this->getSingleComputedStepValue();
  if (this->loopEntryPHIType->isIntegerTy()) {
    auto constantValue = cast<ConstantInt>(stepValue)->getValue();
    return constantValue.isStrictlyPositive();
  }

  auto fpValue = cast<ConstantFP>(stepValue)->getValueAPF();
  return fpValue.isNonZero() && !fpValue.isNegative();
}

const llvm::SCEV *InductionVariable::getStepSCEV(void) const { return this->stepSCEV; }
Type *InductionVariable::getType(void) const { return this->loopEntryPHIType; }
bool InductionVariable::isIVInstruction(Instruction *I) const {
  return this->instructions.count(I) != 0;
}
bool InductionVariable::isDerivedFromIVInstructions(Instruction *I) const {
  return this->derivedInstructions.count(I) != 0;
}

} // namespace loop
} // namespace analysis
} // namespace lotus
