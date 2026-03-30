/*
 * Copyright 2026 Lotus contributors
 */
#include "Analysis/Loop/MemoryCloningAnalysis.h"

namespace lotus {
namespace analysis {
namespace loop {

AllocaInst *ClonableMemoryObject::getAllocation(void) const {
  return this->allocation;
}

uint64_t ClonableMemoryObject::getAllocationSizeInBits(void) const {
  return this->sizeInBits;
}

bool ClonableMemoryObject::mustAliasAMemoryLocationWithinObject(
    Value *pointer) const {
  if (pointer == this->allocation) {
    return true;
  }
  return this->castsAndGEPs.count(dyn_cast_or_null<Instruction>(pointer)) != 0;
}

bool ClonableMemoryObject::isInstructionCastOrGEPOfLocation(Instruction *I) const {
  return this->castsAndGEPs.count(I) != 0;
}

bool ClonableMemoryObject::isInstructionStoringLocation(Instruction *I) const {
  return this->storingInstructions.count(I) != 0;
}

bool ClonableMemoryObject::isInstructionLoadingLocation(Instruction *I) const {
  return this->nonStoringInstructions.count(I) != 0 ||
         this->loadInstructions.count(I) != 0;
}

bool ClonableMemoryObject::isInstructionUsingLocationWithoutStoring(
    Instruction *I) const {
  return this->nonStoringInstructions.count(I) != 0;
}

void ClonableMemoryObject::addPointer(Instruction *I) {
  this->castsAndGEPs.insert(I);
}

void ClonableMemoryObject::addStore(Instruction *I) {
  this->storingInstructions.insert(I);
}

void ClonableMemoryObject::addLoad(Instruction *I) {
  this->loadInstructions.insert(I);
}

void ClonableMemoryObject::addNonStoringUse(Instruction *I) {
  this->nonStoringInstructions.insert(I);
}

std::unordered_set<Instruction *>
ClonableMemoryObject::getLocationPointerInstructions(void) const {
  return this->castsAndGEPs;
}

void ClonableMemoryObject::setClonable(bool clonable) {
  this->clonable = clonable;
}

void ClonableMemoryObject::setNeedsInitialization(bool needsInitialization) {
  this->needsInitialization = needsInitialization;
}

void ClonableMemoryObject::setObjectScope(AllocaInst *allocation,
                                          LoopStructure *loop,
                                          noelle::DominatorSummary &) {
  for (auto *inst : loop->getInstructions()) {
    auto *call = dyn_cast<CallInst>(inst);
    if (call == nullptr || !call->isLifetimeStartOrEnd()) {
      continue;
    }
    Value *objectUsed = call->getArgOperand(1);
    if (auto *castInst = dyn_cast<CastInst>(objectUsed)) {
      objectUsed = castInst->getOperand(0);
    }
    if (objectUsed == allocation) {
      this->scopeWithinLoop = true;
      return;
    }
  }
}

bool ClonableMemoryObject::isMemCpyInstrinsicCall(CallInst *call) {
  if (call == nullptr) {
    return false;
  }
  auto *calledFn = call->getCalledFunction();
  if (calledFn == nullptr || !calledFn->hasName()) {
    return false;
  }
  return calledFn->getName().contains("llvm.memcpy");
}

bool ClonableMemoryObject::identifyStoresAndOtherUsers(
    LoopStructure *loop, noelle::DominatorSummary &DS) {
  std::queue<Instruction *> allocationUses;
  allocationUses.push(this->allocation);
  while (!allocationUses.empty()) {
    auto *I = allocationUses.front();
    allocationUses.pop();

    for (auto *user : I->users()) {
      if (auto *cast = dyn_cast<CastInst>(user)) {
        allocationUses.push(cast);
        this->castsAndGEPs.insert(cast);
        continue;
      }
      if (auto *gep = dyn_cast<GetElementPtrInst>(user)) {
        allocationUses.push(gep);
        this->castsAndGEPs.insert(gep);
        continue;
      }

      auto *inst = dyn_cast<Instruction>(user);
      if (inst == nullptr) {
        return false;
      }

      if (auto *store = dyn_cast<StoreInst>(inst)) {
        this->storingInstructions.insert(store);
      } else if (auto *load = dyn_cast<LoadInst>(inst)) {
        this->loadInstructions.insert(load);
      } else if (auto *call = dyn_cast<CallInst>(inst)) {
        if (call->isLifetimeStartOrEnd()) {
          continue;
        }
        bool isMemCpy = ClonableMemoryObject::isMemCpyInstrinsicCall(call);
        bool isDestinationUse =
            call->arg_size() == 4 && call->getArgOperand(0) == I;
        bool isSourceUse = call->arg_size() == 4 && call->getArgOperand(1) == I;
        if (isMemCpy && isDestinationUse) {
          this->storingInstructions.insert(call);
        } else if (isMemCpy && isSourceUse) {
          this->loadInstructions.insert(call);
        } else {
          this->nonStoringInstructions.insert(call);
        }
      } else {
        this->nonStoringInstructions.insert(inst);
      }

      if (!loop->isIncluded(inst)) {
        auto *block = inst->getParent();
        auto *header = loop->getHeader();
        if (!DS.DT.dominates(block, header)) {
          return false;
        }
      }

      if (isa<InvokeInst>(inst)) {
        return false;
      }
    }
  }

  return true;
}

bool ClonableMemoryObject::isThereAMemoryDependenceBetweenLoopIterations(
    LoopStructure *loop, AllocaInst *, LoopDependenceGraph *ldg,
    const std::unordered_set<Instruction *> &insts) const {
  for (auto *inst : insts) {
    if (!loop->isIncluded(inst)) {
      continue;
    }
    auto functor = [loop](Value *fromValue, LoopDependenceEdge *d) -> bool {
      auto *otherInst = dyn_cast<Instruction>(fromValue);
      if (otherInst == nullptr || !loop->isIncluded(otherInst)) {
        return false;
      }
      return d->isLoopCarried();
    };

    if (ldg->iterateOverDependencesTo(inst, false, false, true, functor) ||
        ldg->iterateOverDependencesFrom(inst, false, false, true, functor)) {
      return true;
    }
  }

  return false;
}

bool ClonableMemoryObject::isThereRAWThroughMemoryBetweenLoopIterations(
    LoopStructure *loop,
    AllocaInst *allocation,
    LoopDependenceGraph *ldg) const {
  return this->isThereRAWThroughMemoryBetweenLoopIterations(
      loop, allocation, ldg, this->loadInstructions);
}

bool ClonableMemoryObject::isThereRAWThroughMemoryBetweenLoopIterations(
    LoopStructure *loop, AllocaInst *, LoopDependenceGraph *ldg,
    const std::unordered_set<Instruction *> &insts) const {
  for (auto *inst : insts) {
    if (!loop->isIncluded(inst)) {
      continue;
    }
    auto functor = [loop](Value *fromValue, LoopDependenceEdge *d) -> bool {
      auto *otherInst = dyn_cast<Instruction>(fromValue);
      if (otherInst == nullptr || !loop->isIncluded(otherInst)) {
        return false;
      }
      return d->getKind() == LoopDependenceEdgeKind::Memory &&
             d->getMemoryKind() == LoopDependenceMemoryKind::Raw &&
             d->isLoopCarried();
    };
    if (ldg->iterateOverDependencesTo(inst, false, false, true, functor)) {
      return true;
    }
  }
  return false;
}

bool ClonableMemoryObject::isThereRAWThroughMemoryFromOutsideToLoop(
    LoopStructure *loop, AllocaInst *allocation, LoopDependenceGraph *ldg) const {
  return this->isThereRAWThroughMemoryFromOutsideToLoop(
      loop, allocation, ldg, this->loadInstructions) ||
         this->isThereRAWThroughMemoryFromOutsideToLoop(
             loop, allocation, ldg, this->nonStoringInstructions);
}

bool ClonableMemoryObject::isThereRAWThroughMemoryFromOutsideToLoop(
    LoopStructure *loop, AllocaInst *, LoopDependenceGraph *ldg,
    std::unordered_set<Instruction *> insts) const {
  for (auto *inst : insts) {
    if (!loop->isIncluded(inst)) {
      continue;
    }
    auto functor = [loop](Value *fromValue, LoopDependenceEdge *d) -> bool {
      auto *otherInst = dyn_cast<Instruction>(fromValue);
      if (otherInst == nullptr || loop->isIncluded(otherInst)) {
        return false;
      }
      return d->getKind() == LoopDependenceEdgeKind::Memory &&
             d->getMemoryKind() == LoopDependenceMemoryKind::Raw;
    };
    if (ldg->iterateOverDependencesTo(inst, false, false, true, functor)) {
      return true;
    }
  }
  return false;
}

bool ClonableMemoryObject::isThereRAWThroughMemoryFromLoopToOutside(
    LoopStructure *loop, AllocaInst *allocation, LoopDependenceGraph *ldg) const {
  return this->isThereRAWThroughMemoryFromLoopToOutside(
      loop, allocation, ldg, this->storingInstructions) ||
         this->isThereRAWThroughMemoryFromLoopToOutside(
             loop, allocation, ldg, this->nonStoringInstructions);
}

bool ClonableMemoryObject::isThereRAWThroughMemoryFromLoopToOutside(
    LoopStructure *loop, AllocaInst *, LoopDependenceGraph *ldg,
    std::unordered_set<Instruction *> insts) const {
  for (auto *inst : insts) {
    if (!loop->isIncluded(inst)) {
      continue;
    }
    auto functor = [loop](Value *toValue, LoopDependenceEdge *d) -> bool {
      auto *otherInst = dyn_cast<Instruction>(toValue);
      if (otherInst == nullptr || loop->isIncluded(otherInst)) {
        return false;
      }
      return d->getKind() == LoopDependenceEdgeKind::Memory &&
             d->getMemoryKind() == LoopDependenceMemoryKind::Raw;
    };
    if (ldg->iterateOverDependencesFrom(inst, false, false, true, functor)) {
      return true;
    }
  }
  return false;
}

bool ClonableMemoryObject::identifyInitialStoringInstructions(
    LoopStructure *loop, noelle::DominatorSummary &DS) {
  std::unordered_set<Instruction *> instructionsNeedingCoverage;
  instructionsNeedingCoverage.insert(this->nonStoringInstructions.begin(),
                                     this->nonStoringInstructions.end());
  instructionsNeedingCoverage.insert(this->loadInstructions.begin(),
                                     this->loadInstructions.end());

  for (auto *instToCover : instructionsNeedingCoverage) {
    auto *instBlock = instToCover->getParent();
    bool belongsToExistingSet = false;
    for (auto &overrideSet : this->overrideSets) {
      auto *dominatingBlock = overrideSet->dominatingBlockOfNonStoringInsts;
      if (DS.DT.dominates(dominatingBlock, instBlock)) {
        overrideSet->subsequentNonStoringInstructions.insert(instToCover);
        belongsToExistingSet = true;
        break;
      }
    }
    if (belongsToExistingSet) {
      continue;
    }
    auto overrideSet = std::unique_ptr<OverrideSet>(new OverrideSet());
    overrideSet->dominatingBlockOfNonStoringInsts = instBlock;
    overrideSet->subsequentNonStoringInstructions.insert(instToCover);
    this->overrideSets.push_back(std::move(overrideSet));
  }

  for (auto *storingInstruction : this->storingInstructions) {
    if (!loop->isIncluded(storingInstruction)) {
      continue;
    }
    auto *storingBlock = storingInstruction->getParent();
    for (auto &overrideSet : this->overrideSets) {
      auto *dominatingBlock = overrideSet->dominatingBlockOfNonStoringInsts;
      if (DS.DT.dominates(storingBlock, dominatingBlock)) {
        overrideSet->initialStoringInstructions.insert(storingInstruction);
      }
    }
  }

  for (auto &overrideSet : this->overrideSets) {
    if (overrideSet->initialStoringInstructions.empty()) {
      return false;
    }
  }

  return true;
}

bool ClonableMemoryObject::areOverrideSetsFullyCoveringTheAllocationSpace(void) const {
  if (this->overrideSets.empty()) {
    return false;
  }
  for (auto const &overrideSet : this->overrideSets) {
    if (!this->isOverrideSetFullyCoveringTheAllocationSpace(overrideSet.get())) {
      return false;
    }
  }
  return true;
}

bool ClonableMemoryObject::isOverrideSetFullyCoveringTheAllocationSpace(
    OverrideSet *overrideSet) const {
  std::unordered_set<int64_t> structElementsStoredTo;
  for (auto *storingInstruction : overrideSet->initialStoringInstructions) {
    if (auto *store = dyn_cast<StoreInst>(storingInstruction)) {
      auto *pointerOperand = store->getPointerOperand();
      if (dyn_cast<AllocaInst>(pointerOperand) != nullptr) {
        return true;
      }
      auto *gep = dyn_cast<GetElementPtrInst>(pointerOperand);
      if (gep == nullptr || !gep->getSourceElementType()->isStructTy() ||
          gep->getSourceElementType() != this->allocatedType) {
        continue;
      }
      auto *baseIdxIter = gep->idx_begin();
      auto *elementIdxIter = baseIdxIter + 1;
      auto *baseIdxValue = dyn_cast<ConstantInt>(baseIdxIter->get());
      auto *elementIdxValue = dyn_cast<ConstantInt>(elementIdxIter->get());
      if (baseIdxValue == nullptr || elementIdxValue == nullptr ||
          baseIdxValue->getSExtValue() != 0) {
        continue;
      }
      structElementsStoredTo.insert(elementIdxValue->getSExtValue());
    } else if (auto *call = dyn_cast<CallInst>(storingInstruction)) {
      if (!ClonableMemoryObject::isMemCpyInstrinsicCall(call)) {
        continue;
      }
      auto *bytesStoredConst = dyn_cast<ConstantInt>(call->getOperand(2));
      if (bytesStoredConst == nullptr) {
        continue;
      }
      if (this->sizeInBits == bytesStoredConst->getZExtValue() * 8) {
        return true;
      }
    }
  }

  if (this->allocatedType != nullptr && this->allocatedType->isStructTy() &&
      structElementsStoredTo.size() == this->allocatedType->getStructNumElements()) {
    return true;
  }
  return false;
}

ClonableMemoryObject::ClonableMemoryObject(AllocaInst *allocation,
                                           uint64_t sizeInBits,
                                           LoopStructure *loop,
                                           noelle::DominatorSummary &DS,
                                           LoopDependenceGraph *ldg)
    : allocation{allocation},
      sizeInBits{sizeInBits},
      loop{loop},
      clonable{false},
      scopeWithinLoop{false},
      needsInitialization{false} {
  this->setObjectScope(allocation, loop, DS);
  this->allocatedType = allocation->getAllocatedType();
  if (!this->identifyStoresAndOtherUsers(loop, DS)) {
    return;
  }

  if (!this->isThereAMemoryDependenceBetweenLoopIterations(
          loop, allocation, ldg, this->storingInstructions) &&
      !this->isThereAMemoryDependenceBetweenLoopIterations(
          loop, allocation, ldg, this->loadInstructions) &&
      !this->isThereAMemoryDependenceBetweenLoopIterations(
          loop, allocation, ldg, this->nonStoringInstructions) &&
      !this->scopeWithinLoop) {
    return;
  }

  if (this->isThereRAWThroughMemoryBetweenLoopIterations(loop, allocation, ldg)) {
    return;
  }

  if (this->scopeWithinLoop) {
    this->clonable = true;
    return;
  }

  if (!this->isThereRAWThroughMemoryFromLoopToOutside(loop, allocation, ldg)) {
    if (!this->isThereRAWThroughMemoryFromOutsideToLoop(loop, allocation, ldg)) {
      this->clonable = true;
      return;
    }

    this->needsInitialization = true;
    this->clonable = true;
    return;
  }

  if (!this->allocatedType->isStructTy() && !this->allocatedType->isIntegerTy()) {
    return;
  }

  this->identifyInitialStoringInstructions(loop, DS);
  if (!this->scopeWithinLoop) {
    if (!this->areOverrideSetsFullyCoveringTheAllocationSpace() ||
        this->isThereRAWThroughMemoryFromLoopToOutside(loop, allocation, ldg)) {
      return;
    }
  }

  this->clonable = true;
}

bool ClonableMemoryObject::isClonableLocation(void) const {
  return this->clonable;
}

bool ClonableMemoryObject::doPrivateCopiesNeedToBeInitialized(void) const {
  return this->needsInitialization;
}

MemoryCloningAnalysis::MemoryCloningAnalysis(LoopStructure *loop,
                                             noelle::DominatorSummary &DS,
                                             LoopDependenceGraph *ldg) {
  assert(loop != nullptr);
  assert(ldg != nullptr);

  auto *function = loop->getFunction();
  auto &entryBlock = function->getEntryBlock();
  auto &DL = function->getParent()->getDataLayout();

  for (auto &I : entryBlock) {
    auto *alloca = dyn_cast<AllocaInst>(&I);
    if (alloca == nullptr) {
      continue;
    }
    auto sizeInBitsOptional = alloca->getAllocationSizeInBits(DL);
    if (!sizeInBitsOptional.hasValue()) {
      continue;
    }

    auto location = std::make_unique<ClonableMemoryObject>(
        alloca, sizeInBitsOptional.getValue(), loop, DS, ldg);
    if (!location->isClonableLocation()) {
      continue;
    }
    this->clonableMemoryLocations.insert(this->clonableMemoryLocations.end(),
                                         std::move(location));
  }
}

std::unordered_set<ClonableMemoryObject *>
MemoryCloningAnalysis::getClonableMemoryObjects(void) const {
  std::unordered_set<ClonableMemoryObject *> locations;
  for (auto const &location : this->clonableMemoryLocations) {
    locations.insert(location.get());
  }
  return locations;
}

std::unordered_set<ClonableMemoryObject *>
MemoryCloningAnalysis::getClonableMemoryObjectsFor(Instruction *I) const {
  std::unordered_set<ClonableMemoryObject *> locations;
  for (auto const &location : this->clonableMemoryLocations) {
    auto *loc = location.get();
    if (loc->getAllocation() == I || loc->isInstructionCastOrGEPOfLocation(I) ||
        loc->isInstructionLoadingLocation(I) ||
        loc->isInstructionStoringLocation(I)) {
      locations.insert(loc);
      continue;
    }
    if (auto *callInst = dyn_cast<CallInst>(I)) {
      if (callInst->isLifetimeStartOrEnd() &&
          callInst->arg_size() > 1 &&
          loc->mustAliasAMemoryLocationWithinObject(callInst->getArgOperand(1))) {
        locations.insert(loc);
      }
    }
  }
  return locations;
}

} // namespace loop
} // namespace analysis
} // namespace lotus
