/*
 * Copyright 2026 Lotus contributors
 */
#include "Analysis/Loop/Variable.h"

namespace lotus {
namespace analysis {
namespace loop {

EvolutionUpdate::EvolutionUpdate(Instruction *updateInstruction)
    : updateInstruction{updateInstruction} {}

bool EvolutionUpdate::mayUpdateBeOverride(void) const {
  if (isa<PHINode>(this->updateInstruction) || isa<SelectInst>(this->updateInstruction)) {
    return true;
  }
  if (isa<CallInst>(this->updateInstruction)) {
    return true;
  }
  if (this->updateInstruction->isBinaryOp()) {
    return false;
  }
  return true;
}

bool EvolutionUpdate::isTransformablyCommutativeWith(
    const EvolutionUpdate &other) const {
  if (this->mayUpdateBeOverride() || other.mayUpdateBeOverride()) {
    return false;
  }
  if (this->isAdd() && other.isAdd()) {
    return true;
  }
  if (this->isMul() && other.isMul()) {
    return true;
  }
  return false;
}

bool EvolutionUpdate::isAssociativeWith(const EvolutionUpdate &other) const {
  return this->isTransformablyCommutativeWith(other);
}

Instruction *EvolutionUpdate::getUpdateInstruction(void) const {
  return this->updateInstruction;
}

bool EvolutionUpdate::isAdd(void) const {
  auto opcode = this->updateInstruction->getOpcode();
  return opcode == Instruction::Add || opcode == Instruction::FAdd;
}

bool EvolutionUpdate::isMul(void) const {
  auto opcode = this->updateInstruction->getOpcode();
  return opcode == Instruction::Mul || opcode == Instruction::FMul;
}

LoopCarriedVariable::LoopCarriedVariable(const LoopStructure &loop,
                                         LoopTree *,
                                         LoopDependenceGraph &,
                                         LoopSCCDAG &,
                                         LoopSCC &variableSCC,
                                         PHINode *declarationPHI)
    : isValid{false},
      outermostLoopOfVariable{loop},
      declarationPHI{declarationPHI},
      initialValue{nullptr},
      accumulator{nullptr},
      reductionOperation{Instruction::BinaryOpsEnd},
      identityValue{nullptr} {
  auto *preHeader = loop.getPreHeader();
  if (preHeader == nullptr || declarationPHI->getBasicBlockIndex(preHeader) < 0) {
    return;
  }
  this->initialValue = declarationPHI->getIncomingValueForBlock(preHeader);

  std::unordered_set<Value *> sccValues;
  for (auto &pair : variableSCC.internalNodePairs()) {
    sccValues.insert(pair.first);
  }

  Instruction *updateInstruction = nullptr;
  for (auto &pair : variableSCC.internalNodePairs()) {
    auto *inst = dyn_cast_or_null<Instruction>(pair.first);
    if (inst == nullptr || inst == declarationPHI) {
      continue;
    }
    if (!inst->isBinaryOp()) {
      return;
    }
    bool usesPhi = false;
    Value *external = nullptr;
    for (auto &operand : inst->operands()) {
      auto *value = operand.get();
      if (value == declarationPHI) {
        usesPhi = true;
      } else if (sccValues.count(value) == 0) {
        external = value;
      }
    }
    if (!usesPhi || external == nullptr) {
      continue;
    }
    if (updateInstruction != nullptr) {
      return;
    }
    updateInstruction = inst;
  }

  if (updateInstruction == nullptr) {
    return;
  }

  if (updateInstruction->getOpcode() != Instruction::Add
      && updateInstruction->getOpcode() != Instruction::FAdd) {
    return;
  }

  this->accumulator = declarationPHI;
  this->reductionOperation =
      static_cast<Instruction::BinaryOps>(updateInstruction->getOpcode());
  this->identityValue =
      declarationPHI->getType()->isFloatingPointTy()
          ? static_cast<Value *>(ConstantFP::get(declarationPHI->getType(), 0.0))
          : static_cast<Value *>(ConstantInt::get(declarationPHI->getType(), 0));
  this->isValid = true;
}

bool LoopCarriedVariable::isEvolutionReducibleAcrossLoopIterations(void) const {
  return this->isValid;
}

PHINode *LoopCarriedVariable::getLoopEntryPHIForValueOfVariable(Value *value) const {
  return value == this->declarationPHI ? this->declarationPHI : nullptr;
}

Value *LoopCarriedVariable::getInitialValue(void) const {
  return this->initialValue;
}

Instruction::BinaryOps LoopCarriedVariable::getReductionOperation(void) const {
  return this->reductionOperation;
}

PHINode *LoopCarriedVariable::getAccumulator(void) const {
  return this->accumulator;
}

Value *LoopCarriedVariable::getIdentityValue(void) const {
  return this->identityValue;
}

} // namespace loop
} // namespace analysis
} // namespace lotus
