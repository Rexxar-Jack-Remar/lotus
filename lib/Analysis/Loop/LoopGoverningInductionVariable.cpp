/*
 * Copyright 2026 Lotus contributors
 */
#include "Analysis/Loop/LoopGoverningInductionVariable.h"

namespace lotus {
namespace analysis {
namespace loop {

LoopGoverningInductionVariable::LoopGoverningInductionVariable(
    LoopStructure *loop,
    InductionVariable &iv)
    : loop{loop},
      iv{&iv},
      headerCmp{nullptr},
      headerBr{nullptr},
      conditionValue{nullptr},
      comparedValue{nullptr},
      exitBlock{nullptr},
      isWellFormed{false} {
  if (loop == nullptr) {
    return;
  }
  auto *header = loop->getHeader();
  auto *branch = dyn_cast<BranchInst>(header->getTerminator());
  if (branch == nullptr || !branch->isConditional()) {
    return;
  }
  auto *cmp = dyn_cast<CmpInst>(branch->getCondition());
  if (cmp == nullptr) {
    return;
  }

  auto *lhs = cmp->getOperand(0);
  auto *rhs = cmp->getOperand(1);
  bool lhsIsIV = lhs == iv.getLoopEntryPHI()
                 || (isa<Instruction>(lhs)
                     && iv.isIVInstruction(cast<Instruction>(lhs)));
  bool rhsIsIV = rhs == iv.getLoopEntryPHI()
                 || (isa<Instruction>(rhs)
                     && iv.isIVInstruction(cast<Instruction>(rhs)));
  if (lhsIsIV == rhsIsIV) {
    return;
  }

  this->headerCmp = cmp;
  this->headerBr = branch;
  this->comparedValue = dyn_cast<Instruction>(lhsIsIV ? lhs : rhs);
  this->conditionValue = lhsIsIV ? rhs : lhs;

  for (auto &edge : loop->getLoopExitEdges()) {
    if (edge.first == header) {
      this->exitBlock = edge.second;
      break;
    }
  }
  if (this->exitBlock == nullptr) {
    return;
  }

  this->isWellFormed = true;
}

InductionVariable *LoopGoverningInductionVariable::getInductionVariable(void) const {
  return this->iv;
}

CmpInst *LoopGoverningInductionVariable::getHeaderCompareInstructionToComputeExitCondition(
    void) const {
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
  if (this->headerBr == nullptr) {
    return false;
  }
  return this->loop->isIncluded(this->headerBr->getSuccessor(0));
}

bool LoopGoverningInductionVariable::isSCCContainingIVWellFormed(void) const {
  return this->isWellFormed;
}

std::set<Instruction *> LoopGoverningInductionVariable::getConditionValueDerivation(
    void) const {
  return {};
}

Instruction *LoopGoverningInductionVariable::getValueToCompareAgainstExitConditionValue(
    void) const {
  return this->comparedValue;
}

} // namespace loop
} // namespace analysis
} // namespace lotus
