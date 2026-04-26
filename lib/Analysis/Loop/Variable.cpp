/*
 * Copyright 2026 Lotus contributors
 */
#include "Analysis/Loop/Variable.h"

#include <queue>

namespace {

using namespace lotus::analysis::loop;

std::unordered_set<Value *> computeSCCContainingValue(
    LoopDependenceGraph &loopDG,
    const std::unordered_set<Value *> &candidateValues, Value *seed,
    const std::function<bool(LoopDependenceEdge *)> &isEdgeAllowed) {
  if (seed == nullptr || candidateValues.count(seed) == 0) {
    return {};
  }

  auto collectReachable = [&](bool forward) {
    std::unordered_set<Value *> reachable;
    std::queue<Value *> worklist;
    reachable.insert(seed);
    worklist.push(seed);

    while (!worklist.empty()) {
      auto *value = worklist.front();
      worklist.pop();

      auto *node = loopDG.getNode(value);
      if (node == nullptr) {
        continue;
      }

      auto const &edges =
          forward ? node->getOutgoingEdges() : node->getIncomingEdges();
      for (auto *edge : edges) {
        if (edge == nullptr || !isEdgeAllowed(edge)) {
          continue;
        }

        auto *nextNode = forward ? edge->getDst() : edge->getSrc();
        if (nextNode == nullptr) {
          continue;
        }

        auto *nextValue = nextNode->getValue();
        if (nextValue == nullptr || candidateValues.count(nextValue) == 0) {
          continue;
        }

        if (reachable.insert(nextValue).second) {
          worklist.push(nextValue);
        }
      }
    }

    return reachable;
  };

  auto forwardReachable = collectReachable(true);
  auto reverseReachable = collectReachable(false);

  std::unordered_set<Value *> scc;
  for (auto *value : forwardReachable) {
    if (reverseReachable.count(value) != 0) {
      scc.insert(value);
    }
  }
  return scc;
}

Value *getIdentityValueForReduction(Type *type, Instruction::BinaryOps op) {
  if (type == nullptr) {
    return nullptr;
  }

  switch (op) {
  case Instruction::Add:
  case Instruction::FAdd:
  case Instruction::Or:
  case Instruction::Xor:
    return Constant::getNullValue(type);
  case Instruction::Mul:
    if (!type->isIntegerTy()) {
      return nullptr;
    }
    return ConstantInt::get(type, 1);
  case Instruction::FMul:
    if (!type->isFloatingPointTy()) {
      return nullptr;
    }
    return ConstantFP::get(type, 1.0);
  case Instruction::And:
    if (!type->isIntegerTy()) {
      return nullptr;
    }
    return Constant::getAllOnesValue(type);
  default:
    return nullptr;
  }
}

Instruction::BinaryOps getReductionOperationForType(Type *type,
                                                    Instruction::BinaryOps op) {
  if (type == nullptr) {
    return Instruction::BinaryOpsEnd;
  }

  switch (op) {
  case Instruction::Sub:
    if (type->isIntegerTy()) {
      return Instruction::Add;
    }
    if (type->isFloatingPointTy()) {
      return Instruction::FAdd;
    }
    return Instruction::BinaryOpsEnd;
  case Instruction::FSub:
    if (type->isFloatingPointTy()) {
      return Instruction::FAdd;
    }
    return Instruction::BinaryOpsEnd;
  default:
    return op;
  }
}

} // namespace

namespace lotus {
namespace analysis {
namespace loop {

EvolutionUpdate::EvolutionUpdate(
    Instruction *updateInstruction,
    const std::unordered_set<Value *> &internalVariableValues)
    : updateInstruction{updateInstruction}, newValue{nullptr},
      internalValuesUsed{}, externalValuesUsed{} {
  if (auto *storeUpdate = dyn_cast<StoreInst>(updateInstruction)) {
    this->newValue = storeUpdate->getValueOperand();
    return;
  }

  this->newValue = updateInstruction;
  for (auto &use : updateInstruction->operands()) {
    auto *usedValue = use.get();
    if (internalVariableValues.count(usedValue) != 0) {
      this->internalValuesUsed.insert(&use);
    } else {
      this->externalValuesUsed.insert(&use);
    }
  }
}

bool EvolutionUpdate::mayUpdateBeOverride(void) const {
  if (isa<SelectInst>(this->updateInstruction) ||
      isa<PHINode>(this->updateInstruction)) {
    return !this->externalValuesUsed.empty();
  }
  if (isa<CallInst>(this->updateInstruction)) {
    return true;
  }
  if (this->updateInstruction->isBinaryOp()) {
    return this->internalValuesUsed.empty();
  }
  if (isa<CmpInst>(this->updateInstruction)) {
    for (auto *user : this->updateInstruction->users()) {
      if (!isa<SelectInst>(user)) {
        return true;
      }
    }
    return false;
  }
  return true;
}

bool EvolutionUpdate::isCommutativeWithSelf(void) const {
  if (this->mayUpdateBeOverride()) {
    return false;
  }
  return this->updateInstruction->isCommutative();
}

bool EvolutionUpdate::isAssociativeWithSelf(void) const {
  if (this->mayUpdateBeOverride()) {
    return false;
  }
  if (this->updateInstruction->isAssociative()) {
    return true;
  }
  if (this->isAdd() || this->isMul()) {
    return true;
  }
  return this->isSubTransformableToAdd();
}

bool EvolutionUpdate::isTransformablyCommutativeWithSelf(void) const {
  if (this->mayUpdateBeOverride()) {
    return false;
  }
  if (this->updateInstruction->isCommutative()) {
    return true;
  }
  return this->isSubTransformableToAdd();
}

bool EvolutionUpdate::isTransformablyCommutativeWith(
    const EvolutionUpdate &other) const {
  if (!this->isTransformablyCommutativeWithSelf() ||
      !other.isTransformablyCommutativeWithSelf()) {
    return false;
  }
  if (this->isBothUpdatesAddOrSub(other)) {
    return true;
  }
  if (this->isBothUpdatesMul(other)) {
    return true;
  }
  if (this->isBothUpdatesSameBitwiseLogicalOp(other)) {
    return true;
  }
  return false;
}

bool EvolutionUpdate::isAssociativeWith(const EvolutionUpdate &other) const {
  if (!this->isAssociativeWithSelf() || !other.isAssociativeWithSelf()) {
    return false;
  }
  if (this->isBothUpdatesAddOrSub(other)) {
    return true;
  }
  if (this->isBothUpdatesMul(other)) {
    return true;
  }
  if (this->isBothUpdatesSameBitwiseLogicalOp(other)) {
    return true;
  }
  return false;
}

Instruction *EvolutionUpdate::getUpdateInstruction(void) const {
  return this->updateInstruction;
}

bool EvolutionUpdate::isAdd(void) const {
  auto op = this->updateInstruction->getOpcode();
  return op == Instruction::Add || op == Instruction::FAdd;
}

bool EvolutionUpdate::isMul(void) const {
  auto op = this->updateInstruction->getOpcode();
  return op == Instruction::Mul || op == Instruction::FMul;
}

bool EvolutionUpdate::isSub(void) const {
  auto op = this->updateInstruction->getOpcode();
  return op == Instruction::Sub || op == Instruction::FSub;
}

bool EvolutionUpdate::isSubTransformableToAdd(void) const {
  if (!this->isSub()) {
    return false;
  }
  auto &useOfValueBeingSubtracted = this->updateInstruction->getOperandUse(1);
  return this->externalValuesUsed.find(&useOfValueBeingSubtracted) !=
         this->externalValuesUsed.end();
}

bool EvolutionUpdate::isBothUpdatesAddOrSub(
    const EvolutionUpdate &other) const {
  auto isThisAddOrSub = this->isAdd() || this->isSubTransformableToAdd();
  auto isOtherAddOrSub = other.isAdd() || other.isSubTransformableToAdd();
  return isThisAddOrSub && isOtherAddOrSub;
}

bool EvolutionUpdate::isBothUpdatesMul(const EvolutionUpdate &other) const {
  return this->isMul() && other.isMul();
}

bool EvolutionUpdate::isBothUpdatesSameBitwiseLogicalOp(
    const EvolutionUpdate &other) const {
  auto thisOp = this->updateInstruction->getOpcode();
  auto otherOp = other.updateInstruction->getOpcode();
  auto isThisLogicalOp = this->updateInstruction->isBitwiseLogicOp();
  auto isOtherLogicalOp = other.updateInstruction->isBitwiseLogicOp();
  return isThisLogicalOp && isOtherLogicalOp && thisOp == otherOp;
}

LoopCarriedVariable::LoopCarriedVariable(
    const LoopStructure &loop, LoopTree *loopNode, LoopDependenceGraph &loopDG,
    LoopSCCDAG &sccdag, LoopSCC &variableSCC, PHINode *declarationPHI)
    : isValid{false}, outermostLoopOfVariable{loop},
      declarationPHI{declarationPHI}, sccOfVariableOnlyValues{},
      sccOfDataAndMemoryVariableValuesOnly{}, controlValuesGoverningEvolution{},
      variableUpdates{}, loopCarriedVariableUpdates{},
      castsInternalToVariableComputation{}, initialValue{nullptr},
      accumulator{nullptr}, reductionOperation{Instruction::BinaryOpsEnd},
      identityValue{nullptr} {
  assert(variableSCC.isInternal(declarationPHI));

  auto *preHeader = this->outermostLoopOfVariable.getPreHeader();
  if (preHeader == nullptr ||
      declarationPHI->getBasicBlockIndex(preHeader) < 0) {
    return;
  }
  this->initialValue = declarationPHI->getIncomingValueForBlock(preHeader);

  auto loopCarriedDependencies =
      LoopCarriedDependencies::getLoopCarriedDependenciesForLoop(loop, loopNode,
                                                                 sccdag);
  std::unordered_set<Value *> loopCarriedValues;
  std::unordered_set<LoopDependenceEdge *> loopCarriedDependenciesNotOfVariable;
  for (auto *dependency : loopCarriedDependencies) {
    auto *consumer = dependency->getDst()->getValue();
    if (consumer == declarationPHI) {
      loopCarriedValues.insert(dependency->getSrc()->getValue());
    } else {
      loopCarriedDependenciesNotOfVariable.insert(dependency);
    }
  }

  std::unordered_set<Value *> allPossibleInternalValues;
  for (auto &nodePair : variableSCC.internalNodePairs()) {
    if (nodePair.first != nullptr) {
      allPossibleInternalValues.insert(nodePair.first);
    }
  }

  auto variableEdgeIsAllowed = [&](LoopDependenceEdge *edge) -> bool {
    return loopCarriedDependenciesNotOfVariable.count(edge) == 0;
  };
  this->sccOfVariableOnlyValues =
      computeSCCContainingValue(loopDG, allPossibleInternalValues,
                                this->declarationPHI, variableEdgeIsAllowed);
  if (this->sccOfVariableOnlyValues.empty()) {
    return;
  }

  this->sccOfDataAndMemoryVariableValuesOnly =
      this->produceDataAndMemoryOnlySCC(loopDG,
                                        loopCarriedDependenciesNotOfVariable);
  if (this->sccOfDataAndMemoryVariableValuesOnly.empty()) {
    return;
  }

  this->collectControlValuesGoverningEvolution(
      loopDG, loopCarriedDependenciesNotOfVariable);
  if (!this->collectVariableUpdates(loopCarriedValues)) {
    return;
  }

  this->accumulator = this->declarationPHI;
  this->isValid = true;
}

LoopCarriedVariable::~LoopCarriedVariable() {
  for (auto *update : this->variableUpdates) {
    delete update;
  }
}

std::unordered_set<Value *> LoopCarriedVariable::produceDataAndMemoryOnlySCC(
    LoopDependenceGraph &loopDG,
    const std::unordered_set<LoopDependenceEdge *>
        &loopCarriedDependenciesNotOfVariable) const {
  std::unordered_set<Value *> values;
  for (auto *value : this->sccOfVariableOnlyValues) {
    auto *node = loopDG.getNode(value);
    if (node == nullptr) {
      continue;
    }
    bool producesControlDependency = false;
    for (auto *edge : node->getOutgoingEdges()) {
      if (loopCarriedDependenciesNotOfVariable.count(edge) != 0) {
        continue;
      }
      auto *dst = edge->getDst();
      if (dst == nullptr ||
          this->sccOfVariableOnlyValues.count(dst->getValue()) == 0) {
        continue;
      }
      if (edge->getKind() == LoopDependenceEdgeKind::Control) {
        producesControlDependency = true;
        break;
      }
    }
    if (!producesControlDependency) {
      values.insert(value);
    }
  }

  if (values.count(this->declarationPHI) == 0) {
    return {};
  }

  auto dataMemoryEdgeIsAllowed = [&](LoopDependenceEdge *edge) -> bool {
    if (loopCarriedDependenciesNotOfVariable.count(edge) != 0) {
      return false;
    }
    return edge->getKind() != LoopDependenceEdgeKind::Control;
  };

  return computeSCCContainingValue(loopDG, values, this->declarationPHI,
                                   dataMemoryEdgeIsAllowed);
}

void LoopCarriedVariable::collectControlValuesGoverningEvolution(
    LoopDependenceGraph &loopDG, const std::unordered_set<LoopDependenceEdge *>
                                     &loopCarriedDependenciesNotOfVariable) {
  for (auto *value : this->sccOfVariableOnlyValues) {
    if (auto *selectInst = dyn_cast<SelectInst>(value)) {
      this->controlValuesGoverningEvolution.insert(selectInst->getCondition());
      continue;
    }

    auto *node = loopDG.getNode(value);
    if (node == nullptr) {
      continue;
    }
    for (auto *edge : node->getOutgoingEdges()) {
      if (loopCarriedDependenciesNotOfVariable.count(edge) != 0) {
        continue;
      }
      auto *dst = edge->getDst();
      if (dst == nullptr ||
          this->sccOfVariableOnlyValues.count(dst->getValue()) == 0) {
        continue;
      }
      if (edge->getKind() == LoopDependenceEdgeKind::Control) {
        this->controlValuesGoverningEvolution.insert(value);
        break;
      }
    }
  }
}

bool LoopCarriedVariable::collectVariableUpdates(
    const std::unordered_set<Value *> &loopCarriedValues) {
  for (auto *value : this->sccOfDataAndMemoryVariableValuesOnly) {
    if (value == this->declarationPHI) {
      continue;
    }
    auto *inst = dyn_cast<Instruction>(value);
    if (inst == nullptr) {
      return false;
    }
    if (isa<LoadInst>(inst)) {
      continue;
    }
    if (auto *castInst = dyn_cast<CastInst>(inst)) {
      this->castsInternalToVariableComputation.insert(castInst);
      continue;
    }

    auto *update =
        new EvolutionUpdate(inst, this->sccOfDataAndMemoryVariableValuesOnly);
    this->variableUpdates.insert(update);
    if (loopCarriedValues.count(value) != 0) {
      this->loopCarriedVariableUpdates.insert(update);
    }
  }
  return true;
}

bool LoopCarriedVariable::isEvolutionReducibleAcrossLoopIterations(void) const {
  if (!this->isValid) {
    return false;
  }

  for (auto *controlValue : this->controlValuesGoverningEvolution) {
    if (this->sccOfVariableOnlyValues.count(controlValue) != 0) {
      return false;
    }
  }

  std::unordered_set<EvolutionUpdate *> arithmeticUpdates;
  for (auto *update : this->variableUpdates) {
    if (update->mayUpdateBeOverride()) {
      return false;
    }
    auto *updateInstruction = update->getUpdateInstruction();
    if (isa<PHINode>(updateInstruction) || isa<SelectInst>(updateInstruction)) {
      continue;
    }
    arithmeticUpdates.insert(update);
  }

  if (this->hasRoundingError(arithmeticUpdates)) {
    return false;
  }
  if (arithmeticUpdates.empty()) {
    return false;
  }

  for (auto *update : arithmeticUpdates) {
    for (auto *otherUpdate : arithmeticUpdates) {
      if (!update->isTransformablyCommutativeWith(*otherUpdate) ||
          !update->isAssociativeWith(*otherUpdate)) {
        return false;
      }
    }
  }

  auto consumers = this->getConsumersOfVariable();
  if (!this->areValuesPropagatingVariableIntermediatesOutsideLoop(consumers)) {
    return false;
  }

  auto operation = Instruction::BinaryOpsEnd;
  for (auto *update : arithmeticUpdates) {
    auto currentOperation = getReductionOperationForType(
        this->declarationPHI->getType(),
        static_cast<Instruction::BinaryOps>(
            update->getUpdateInstruction()->getOpcode()));
    if (currentOperation == Instruction::BinaryOpsEnd) {
      return false;
    }
    if (operation == Instruction::BinaryOpsEnd) {
      operation = currentOperation;
      continue;
    }
    if (operation != currentOperation) {
      return false;
    }
  }

  auto *identity =
      getIdentityValueForReduction(this->declarationPHI->getType(), operation);
  if (identity == nullptr) {
    return false;
  }

  this->reductionOperation = operation;
  this->identityValue = identity;

  return true;
}

std::unordered_set<Value *>
LoopCarriedVariable::getConsumersOfVariable(void) const {
  std::unordered_set<Value *> consumers;
  for (auto *value : this->sccOfVariableOnlyValues) {
    for (auto *user : value->users()) {
      auto *consumer = dyn_cast<Instruction>(user);
      if (consumer == nullptr ||
          !this->outermostLoopOfVariable.isIncluded(consumer) ||
          this->sccOfVariableOnlyValues.count(consumer) != 0) {
        continue;
      }
      consumers.insert(consumer);
    }
  }
  return consumers;
}

bool LoopCarriedVariable::areValuesPropagatingVariableIntermediatesOutsideLoop(
    const std::unordered_set<Value *> &values) const {
  auto *loopHeader = this->outermostLoopOfVariable.getHeader();
  auto *loopPreheader = this->outermostLoopOfVariable.getPreHeader();
  std::queue<Value *> valuesToCheck;
  std::unordered_set<Value *> valuesChecked;
  for (auto *value : values) {
    valuesToCheck.push(value);
    valuesChecked.insert(value);
  }

  while (!valuesToCheck.empty()) {
    auto *value = valuesToCheck.front();
    valuesToCheck.pop();

    if (auto *castInst = dyn_cast<CastInst>(value)) {
      auto *valueToCast = castInst->getOperand(0);
      if (this->sccOfDataAndMemoryVariableValuesOnly.count(valueToCast) == 0) {
        return false;
      }
    } else if (auto *phi = dyn_cast<PHINode>(value)) {
      if (loopHeader != phi->getParent()) {
        return false;
      }

      Value *singleIncomingValue = nullptr;
      for (unsigned i = 0; i < phi->getNumIncomingValues(); ++i) {
        auto *incomingBlock = phi->getIncomingBlock(i);
        auto *incomingValue = phi->getIncomingValue(i);
        if (incomingBlock == loopPreheader) {
          continue;
        }
        if (singleIncomingValue == nullptr ||
            incomingValue == singleIncomingValue) {
          singleIncomingValue = incomingValue;
          continue;
        }
        singleIncomingValue = nullptr;
        break;
      }

      if (singleIncomingValue == nullptr ||
          this->sccOfDataAndMemoryVariableValuesOnly.count(
              singleIncomingValue) == 0) {
        return false;
      }
    } else {
      return false;
    }

    for (auto *user : value->users()) {
      auto *userI = dyn_cast<Instruction>(user);
      if (userI == nullptr ||
          !this->outermostLoopOfVariable.isIncluded(userI->getParent()) ||
          valuesChecked.count(userI) != 0) {
        continue;
      }
      valuesToCheck.push(userI);
      valuesChecked.insert(userI);
    }
  }

  return true;
}

PHINode *
LoopCarriedVariable::getLoopEntryPHIForValueOfVariable(Value *value) const {
  if (this->sccOfVariableOnlyValues.count(value) == 0) {
    return dyn_cast<PHINode>(value);
  }
  return this->declarationPHI;
}

bool LoopCarriedVariable::hasRoundingError(
    std::unordered_set<EvolutionUpdate *> &arithmeticUpdates) const {
  bool isIntegerTypedCast = false;
  bool isFloatingPointTypedCast = false;
  for (auto *castInst : this->castsInternalToVariableComputation) {
    auto *castTy = castInst->getType();
    isIntegerTypedCast |= castTy->isIntegerTy();
    isFloatingPointTypedCast |= castTy->isFloatingPointTy();

    auto *srcType = castInst->getSrcTy();
    if (castTy->isFloatingPointTy() && srcType->isFloatingPointTy()) {
      return true;
    }
  }
  if (!isIntegerTypedCast || !isFloatingPointTypedCast) {
    return false;
  }

  auto *accumulationType = this->declarationPHI->getType();
  bool onlyAddition = true;
  for (auto *update : arithmeticUpdates) {
    onlyAddition &= update->isAdd() || update->isSubTransformableToAdd();
  }
  if (accumulationType->isIntegerTy() && onlyAddition) {
    return false;
  }
  return true;
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
