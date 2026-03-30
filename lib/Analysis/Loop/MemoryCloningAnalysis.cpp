/*
 * Copyright 2026 Lotus contributors
 */
#include "Analysis/Loop/MemoryCloningAnalysis.h"

namespace lotus {
namespace analysis {
namespace loop {

namespace {

Value *stripLocationCasts(Value *value) {
  Value *current = value;
  while (true) {
    if (auto *gep = dyn_cast<GetElementPtrInst>(current)) {
      current = gep->getPointerOperand();
      continue;
    }
    if (auto *bitcast = dyn_cast<BitCastInst>(current)) {
      current = bitcast->getOperand(0);
      continue;
    }
    if (auto *addrspace = dyn_cast<AddrSpaceCastInst>(current)) {
      current = addrspace->getOperand(0);
      continue;
    }
    break;
  }
  return current;
}

} // namespace

ClonableMemoryObject::ClonableMemoryObject(AllocaInst *allocation, uint64_t sizeInBits)
    : allocation{allocation},
      sizeInBits{sizeInBits},
      clonable{false},
      needsInitialization{false} {}

AllocaInst *ClonableMemoryObject::getAllocation(void) const { return this->allocation; }
bool ClonableMemoryObject::isClonableLocation(void) const { return this->clonable; }
bool ClonableMemoryObject::doPrivateCopiesNeedToBeInitialized(void) const {
  return this->needsInitialization;
}
bool ClonableMemoryObject::mustAliasAMemoryLocationWithinObject(Value *pointer) const {
  return stripLocationCasts(pointer) == this->allocation;
}
bool ClonableMemoryObject::isInstructionCastOrGEPOfLocation(Instruction *I) const {
  return this->castsAndGEPs.count(I) != 0;
}
bool ClonableMemoryObject::isInstructionStoringLocation(Instruction *I) const {
  return this->storingInstructions.count(I) != 0;
}
bool ClonableMemoryObject::isInstructionLoadingLocation(Instruction *I) const {
  return this->loadInstructions.count(I) != 0;
}
void ClonableMemoryObject::addPointer(Instruction *I) { this->castsAndGEPs.insert(I); }
void ClonableMemoryObject::addStore(Instruction *I) { this->storingInstructions.insert(I); }
void ClonableMemoryObject::addLoad(Instruction *I) { this->loadInstructions.insert(I); }
void ClonableMemoryObject::setClonable(bool clonable) { this->clonable = clonable; }
void ClonableMemoryObject::setNeedsInitialization(bool needsInitialization) {
  this->needsInitialization = needsInitialization;
}

MemoryCloningAnalysis::MemoryCloningAnalysis(LoopStructure *loop,
                                             noelle::DominatorSummary &,
                                             LoopDependenceGraph *ldg) {
  assert(loop != nullptr);
  assert(ldg != nullptr);

  auto *function = loop->getFunction();
  auto &entryBlock = function->getEntryBlock();
  auto &DL = function->getParent()->getDataLayout();

  for (auto &inst : entryBlock) {
    auto *alloca = dyn_cast<AllocaInst>(&inst);
    if (alloca == nullptr) {
      continue;
    }
    auto sizeInBitsOptional = alloca->getAllocationSizeInBits(DL);
    if (!sizeInBitsOptional.hasValue()) {
      continue;
    }

    auto object = std::make_unique<ClonableMemoryObject>(
        alloca, sizeInBitsOptional.getValue());

    bool hasLoopUse = false;
    bool hasIllegalOutsideUse = false;
    bool hasOutsideInitializationStore = false;
    bool hasLoopLoad = false;

    std::queue<Value *> worklist;
    std::unordered_set<Value *> visited;
    worklist.push(alloca);
    visited.insert(alloca);

    while (!worklist.empty()) {
      auto *value = worklist.front();
      worklist.pop();
      for (auto *user : value->users()) {
        if (!visited.insert(user).second) {
          continue;
        }

        auto *userInst = dyn_cast<Instruction>(user);
        if (userInst == nullptr) {
          hasIllegalOutsideUse = true;
          continue;
        }

        if (isa<GetElementPtrInst>(userInst) || isa<BitCastInst>(userInst)
            || isa<AddrSpaceCastInst>(userInst)) {
          object->addPointer(userInst);
          worklist.push(userInst);
        }

        if (auto *load = dyn_cast<LoadInst>(userInst)) {
          object->addLoad(load);
          if (loop->isIncluded(load)) {
            hasLoopLoad = true;
          }
        }
        if (auto *store = dyn_cast<StoreInst>(userInst)) {
          if (object->mustAliasAMemoryLocationWithinObject(store->getPointerOperand())) {
            object->addStore(store);
          }
        }
        if (auto *call = dyn_cast<CallInst>(userInst)) {
          if (call->isLifetimeStartOrEnd()) {
            continue;
          }
          if (auto intrinsic = call->getIntrinsicID();
              intrinsic == Intrinsic::memcpy || intrinsic == Intrinsic::memmove) {
            hasLoopUse |= loop->isIncluded(call);
            if (!loop->isIncluded(call)) {
              hasIllegalOutsideUse = true;
            }
            continue;
          }
        }

        if (loop->isIncluded(userInst)) {
          hasLoopUse = true;
          continue;
        }

        if (auto *store = dyn_cast<StoreInst>(userInst)) {
          if (object->mustAliasAMemoryLocationWithinObject(store->getPointerOperand())) {
            hasOutsideInitializationStore = true;
            continue;
          }
        }

        if (isa<GetElementPtrInst>(userInst) || isa<BitCastInst>(userInst)
            || userInst->isLifetimeStartOrEnd()) {
          continue;
        }

        hasIllegalOutsideUse = true;
      }
    }

    object->setNeedsInitialization(hasOutsideInitializationStore && hasLoopLoad);
    object->setClonable(hasLoopUse && !hasIllegalOutsideUse);
    if (object->isClonableLocation()) {
      this->clonableMemoryLocations.push_back(std::move(object));
    }
  }
}

std::unordered_set<ClonableMemoryObject *>
MemoryCloningAnalysis::getClonableMemoryObjects(void) const {
  std::unordered_set<ClonableMemoryObject *> result;
  for (auto const &location : this->clonableMemoryLocations) {
    result.insert(location.get());
  }
  return result;
}

std::unordered_set<ClonableMemoryObject *>
MemoryCloningAnalysis::getClonableMemoryObjectsFor(Instruction *I) const {
  std::unordered_set<ClonableMemoryObject *> result;
  for (auto const &location : this->clonableMemoryLocations) {
    auto *loc = location.get();
    if (loc->getAllocation() == I || loc->isInstructionCastOrGEPOfLocation(I)
        || loc->isInstructionLoadingLocation(I)
        || loc->isInstructionStoringLocation(I)) {
      result.insert(loc);
      continue;
    }
    if (auto *callInst = dyn_cast<CallInst>(I)) {
      if (callInst->isLifetimeStartOrEnd()
          && callInst->arg_size() > 1
          && loc->mustAliasAMemoryLocationWithinObject(callInst->getArgOperand(1))) {
        result.insert(loc);
        continue;
      }
      auto intrinsic = callInst->getIntrinsicID();
      if ((intrinsic == Intrinsic::memcpy || intrinsic == Intrinsic::memmove)
          && callInst->arg_size() > 1
          && (loc->mustAliasAMemoryLocationWithinObject(callInst->getArgOperand(0))
              || loc->mustAliasAMemoryLocationWithinObject(callInst->getArgOperand(1)))) {
        result.insert(loc);
        continue;
      }
      for (auto &arg : callInst->args()) {
        if (loc->mustAliasAMemoryLocationWithinObject(arg.get())) {
          result.insert(loc);
          break;
        }
      }
    }
  }
  return result;
}

} // namespace loop
} // namespace analysis
} // namespace lotus
