/*
 * Copyright 2026 Lotus contributors
 */
#include "Analysis/Loop/MemoryCloningAnalysis.h"

namespace lotus {
namespace analysis {
namespace loop {

ClonableMemoryObject::ClonableMemoryObject(AllocaInst *allocation)
    : allocation{allocation}, clonable{false} {}

AllocaInst *ClonableMemoryObject::getAllocation(void) const { return this->allocation; }
bool ClonableMemoryObject::isClonableLocation(void) const { return this->clonable; }
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

MemoryCloningAnalysis::MemoryCloningAnalysis(LoopStructure *loop,
                                             LoopDependenceGraph *ldg) {
  assert(loop != nullptr);
  assert(ldg != nullptr);

  auto &entryBlock = loop->getFunction()->getEntryBlock();
  for (auto &inst : entryBlock) {
    auto *alloca = dyn_cast<AllocaInst>(&inst);
    if (alloca == nullptr) {
      continue;
    }

    auto object = std::make_unique<ClonableMemoryObject>(alloca);
    bool usedInLoop = false;
    bool usedOutsideLoop = false;
    for (auto *user : alloca->users()) {
      auto *userInst = dyn_cast<Instruction>(user);
      if (userInst == nullptr) {
        usedOutsideLoop = true;
        break;
      }

      if (isa<GetElementPtrInst>(userInst) || isa<BitCastInst>(userInst)) {
        object->addPointer(userInst);
      }
      if (auto *load = dyn_cast<LoadInst>(userInst)) {
        object->addLoad(load);
      }
      if (auto *store = dyn_cast<StoreInst>(userInst)) {
        object->addStore(store);
      }

      if (loop->isIncluded(userInst)) {
        usedInLoop = true;
      } else if (isa<StoreInst>(userInst) || isa<GetElementPtrInst>(userInst)
                 || isa<BitCastInst>(userInst)
                 || userInst->isLifetimeStartOrEnd()) {
        continue;
      } else {
        usedOutsideLoop = true;
      }
    }

    object->setClonable(usedInLoop && !usedOutsideLoop);
    if (object->isClonableLocation()) {
      this->clonableMemoryLocations.push_back(std::move(object));
    }
  }
}

std::unordered_set<ClonableMemoryObject *> MemoryCloningAnalysis::getClonableMemoryObjects(
    void) const {
  std::unordered_set<ClonableMemoryObject *> result;
  for (auto const &location : this->clonableMemoryLocations) {
    result.insert(location.get());
  }
  return result;
}

std::unordered_set<ClonableMemoryObject *> MemoryCloningAnalysis::getClonableMemoryObjectsFor(
    Instruction *I) const {
  std::unordered_set<ClonableMemoryObject *> result;
  for (auto const &location : this->clonableMemoryLocations) {
    auto *loc = location.get();
    if (loc->getAllocation() == I
        || loc->isInstructionCastOrGEPOfLocation(I)
        || loc->isInstructionLoadingLocation(I)
        || loc->isInstructionStoringLocation(I)) {
      result.insert(loc);
    }
  }
  return result;
}

} // namespace loop
} // namespace analysis
} // namespace lotus
