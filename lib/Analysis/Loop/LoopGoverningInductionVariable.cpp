/*
 * Copyright 2026 Lotus contributors
 */
#include "Analysis/Loop/LoopGoverningInductionVariable.h"

namespace lotus {
namespace analysis {
namespace loop {

LoopGoverningInductionVariable::LoopGoverningInductionVariable(
    LoopStructure *loop, InductionVariable &iv)
    : LoopGoverningInductionVariable(loop, iv, loop->getLoopExitBasicBlocks()) {
}

LoopGoverningInductionVariable::LoopGoverningInductionVariable(
    LoopStructure *loop, InductionVariable &iv, LoopSCC &,
    const std::vector<BasicBlock *> &exitBlocks)
    : loop{loop}, iv{&iv}, conditionValueDerivation{}, headerCmp{nullptr},
      headerBr{nullptr}, conditionValue{nullptr}, comparedValue{nullptr},
      exitBlock{nullptr}, isWellFormed{false} {
  assert(loop != nullptr);

  if (iv.getSingleComputedStepValue() == nullptr ||
      (!isa<ConstantInt>(iv.getSingleComputedStepValue()) &&
       !isa<ConstantFP>(iv.getSingleComputedStepValue()))) {
    return;
  }

  auto *headerPHI = iv.getLoopEntryPHI();
  auto ivInstructions = iv.getAllInstructions();
  if (!headerPHI->getType()->isIntegerTy() &&
      !headerPHI->getType()->isFloatingPointTy()) {
    return;
  }

  auto *sccOfIV = iv.getSCC();
  if (sccOfIV == nullptr) {
    return;
  }

  BranchInst *loopGoverningTerminator = nullptr;
  for (auto &pair : sccOfIV->internalNodePairs()) {
    auto *value = pair.first;
    if (isa<InvokeInst>(value)) {
      return;
    }
    auto *br = dyn_cast<BranchInst>(value);
    if (br == nullptr || !br->isConditional()) {
      continue;
    }
    if (loopGoverningTerminator != nullptr) {
      return;
    }
    loopGoverningTerminator = br;
  }

  if (loopGoverningTerminator == nullptr ||
      loopGoverningTerminator->getParent() != headerPHI->getParent()) {
    return;
  }

  this->headerBr = loopGoverningTerminator;
  auto *headerCondition = this->headerBr->getCondition();
  auto *cmp = dyn_cast<CmpInst>(headerCondition);
  if (cmp == nullptr) {
    return;
  }

  this->headerCmp = cmp;
  auto *opL = cmp->getOperand(0);
  auto *opR = cmp->getOperand(1);
  auto isOpLHSLoopEntryPHI =
      isa<Instruction>(opL) && headerPHI == cast<Instruction>(opL);
  auto isOpRHSLoopEntryPHI =
      isa<Instruction>(opR) && headerPHI == cast<Instruction>(opR);
  if (!(isOpLHSLoopEntryPHI ^ isOpRHSLoopEntryPHI)) {
    for (auto *intermediateValue : iv.getNonPHIIntermediateValues()) {
      if (intermediateValue == opR || intermediateValue == opL) {
        this->comparedValue = intermediateValue;
        break;
      }
    }
    if (this->comparedValue == nullptr) {
      return;
    }
    this->conditionValue = (this->comparedValue == opR) ? opL : opR;
  } else {
    this->conditionValue = isOpLHSLoopEntryPHI ? opR : opL;
    this->comparedValue = cast<Instruction>(isOpLHSLoopEntryPHI ? opL : opR);
  }

  std::set<BasicBlock *> exitBlockSet(exitBlocks.begin(), exitBlocks.end());
  if (exitBlockSet.count(this->headerBr->getSuccessor(0)) != 0) {
    this->exitBlock = this->headerBr->getSuccessor(0);
  } else if (exitBlockSet.count(this->headerBr->getSuccessor(1)) != 0) {
    this->exitBlock = this->headerBr->getSuccessor(1);
  } else {
    return;
  }

  if (sccOfIV->isInternal(this->conditionValue)) {
    auto *conditionInst = dyn_cast<Instruction>(this->conditionValue);
    if (conditionInst == nullptr) {
      return;
    }
    std::queue<Instruction *> conditionDerivation;
    conditionDerivation.push(conditionInst);
    while (!conditionDerivation.empty()) {
      auto *value = conditionDerivation.front();
      conditionDerivation.pop();

      auto *valueNode = sccOfIV->fetchNode(value);
      if (valueNode == nullptr) {
        continue;
      }
      for (auto *edge : valueNode->getIncomingEdges()) {
        if (edge->getKind() != LoopDependenceEdgeKind::Variable) {
          continue;
        }
        auto *outgoingInst =
            dyn_cast_or_null<Instruction>(edge->getSrc()->getValue());
        if (outgoingInst == nullptr || !sccOfIV->isInternal(outgoingInst)) {
          continue;
        }
        if (ivInstructions.count(outgoingInst) != 0) {
          return;
        }
        if (!this->conditionValueDerivation.insert(outgoingInst).second) {
          continue;
        }
        conditionDerivation.push(outgoingInst);
      }
    }
  }

  this->isWellFormed = true;
}

LoopGoverningInductionVariable::LoopGoverningInductionVariable(
    LoopStructure *loop, InductionVariable &iv,
    const std::vector<BasicBlock *> &exitBlocks)
    : loop{loop}, iv{&iv}, conditionValueDerivation{}, headerCmp{nullptr},
      headerBr{nullptr}, conditionValue{nullptr}, comparedValue{nullptr},
      exitBlock{nullptr}, isWellFormed{false} {
  auto *scc = iv.getSCC();
  if (scc == nullptr) {
    return;
  }

  LoopGoverningInductionVariable scoped(loop, iv, *scc, exitBlocks);
  this->conditionValueDerivation = scoped.getConditionValueDerivation();
  this->headerCmp = scoped.getHeaderCompareInstructionToComputeExitCondition();
  this->headerBr = scoped.getHeaderBrInst();
  this->conditionValue = scoped.getExitConditionValue();
  this->comparedValue = scoped.getValueToCompareAgainstExitConditionValue();
  this->exitBlock = scoped.getExitBlockFromHeader();
  this->isWellFormed = scoped.isSCCContainingIVWellFormed();
}

InductionVariable *
LoopGoverningInductionVariable::getInductionVariable(void) const {
  return this->iv;
}

CmpInst *LoopGoverningInductionVariable::
    getHeaderCompareInstructionToComputeExitCondition(void) const {
  return this->headerCmp;
}

Value *LoopGoverningInductionVariable::getExitConditionValue(void) const {
  return this->conditionValue;
}

BranchInst *LoopGoverningInductionVariable::getHeaderBrInst(void) const {
  return this->headerBr;
}

BasicBlock *LoopGoverningInductionVariable::getExitBlockFromHeader(void) const {
  return this->exitBlock;
}

bool LoopGoverningInductionVariable::valueOfExitConditionToJumpToTheLoopBody(
    void) const {
  assert(this->headerBr != nullptr);
  auto *succTrue = this->headerBr->getSuccessor(0);
  return this->loop->isIncluded(succTrue);
}

bool LoopGoverningInductionVariable::isSCCContainingIVWellFormed(void) const {
  return this->isWellFormed;
}

std::set<Instruction *>
LoopGoverningInductionVariable::getConditionValueDerivation(void) const {
  return this->conditionValueDerivation;
}

Instruction *
LoopGoverningInductionVariable::getValueToCompareAgainstExitConditionValue(
    void) const {
  return this->comparedValue;
}

} // namespace loop
} // namespace analysis
} // namespace lotus
