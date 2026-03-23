/**
 * @file RAIILockTracker.cpp
 * @brief RAII Lock Lifetime Tracking Implementation
 *
 * @author rainoftime
 * @date 2026
 */

#include "Analysis/Concurrency/Utils/RAIILockTracker.h"

#include "Analysis/Concurrency/Utils/CppThreading.h"

#include <algorithm>
#include <deque>
#include <unordered_set>

#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>

namespace RAIILock {

namespace {

const llvm::Instruction *
findImpreciseLifetimeBoundary(const llvm::AllocaInst *lockAlloca,
                              const llvm::Function *F) {
  if (!lockAlloca || !F) {
    return nullptr;
  }

  std::deque<const llvm::Value *> worklist;
  std::unordered_set<const llvm::Value *> visited;
  std::unordered_set<const llvm::Instruction *> use_insts;
  const llvm::Instruction *last_use = nullptr;

  worklist.push_back(lockAlloca);
  visited.insert(lockAlloca);

  while (!worklist.empty()) {
    const llvm::Value *current = worklist.front();
    worklist.pop_front();

    for (const llvm::User *user : current->users()) {
      const llvm::Value *derived = llvm::dyn_cast<llvm::Value>(user);
      if (!derived || !visited.insert(derived).second) {
        continue;
      }

      if (const auto *inst = llvm::dyn_cast<llvm::Instruction>(user)) {
        use_insts.insert(inst);
      }

      if (llvm::isa<llvm::BitCastInst>(user) || llvm::isa<llvm::GetElementPtrInst>(user) ||
          llvm::isa<llvm::PHINode>(user) || llvm::isa<llvm::SelectInst>(user)) {
        worklist.push_back(derived);
      }
    }
  }

  for (llvm::const_inst_iterator I = llvm::inst_begin(F), E = llvm::inst_end(F);
       I != E; ++I) {
    const llvm::Instruction *inst = &*I;
    if (use_insts.count(inst) != 0) {
      last_use = inst;
    }
  }

  return last_use ? last_use->getNextNode() : nullptr;
}

} // namespace

bool RAIILockTracker::isRAIILockConstructor(const llvm::Instruction *inst) {
  const auto *call = llvm::dyn_cast<llvm::CallBase>(inst);
  if (!call)
    return false;

  const auto *func = call->getCalledFunction();
  if (!func) {
    func = llvm::dyn_cast<llvm::Function>(
        call->getCalledOperand()->stripPointerCasts());
  }
  if (!func || !func->hasName())
    return false;

  llvm::StringRef name = func->getName();
  return CppThreadingModel::isLockGuardConstructor(name) ||
         CppThreadingModel::isUniqueLockConstructor(name) ||
         CppThreadingModel::isScopedLockConstructor(name) ||
         CppThreadingModel::isSharedLockConstructor(name);
}

bool RAIILockTracker::isRAIILockDestructor(const llvm::Instruction *inst) {
  const auto *call = llvm::dyn_cast<llvm::CallBase>(inst);
  if (!call)
    return false;

  const auto *func = call->getCalledFunction();
  if (!func) {
    func = llvm::dyn_cast<llvm::Function>(
        call->getCalledOperand()->stripPointerCasts());
  }
  if (!func || !func->hasName())
    return false;

  llvm::StringRef name = func->getName();
  return CppThreadingModel::isLockGuardDestructor(name) ||
         CppThreadingModel::isUniqueLockDestructor(name) ||
         CppThreadingModel::isScopedLockDestructor(name) ||
         CppThreadingModel::isSharedLockDestructor(name);
}

bool RAIILockTracker::isSharedLock(const llvm::Instruction *inst) {
  const auto *call = llvm::dyn_cast<llvm::CallBase>(inst);
  if (!call)
    return false;

  const auto *func = call->getCalledFunction();
  if (!func) {
    func = llvm::dyn_cast<llvm::Function>(
        call->getCalledOperand()->stripPointerCasts());
  }
  if (!func || !func->hasName())
    return false;

  return CppThreadingModel::isSharedLockConstructor(func->getName());
}

OwnershipKind RAIILockTracker::getOwnershipKind(const llvm::CallBase *ctor) {
  if (!ctor) {
    return OwnershipKind::Unknown;
  }

  const auto *func = ctor->getCalledFunction();
  if (!func) {
    func = llvm::dyn_cast<llvm::Function>(
        ctor->getCalledOperand()->stripPointerCasts());
  }
  if (!func || !func->hasName()) {
    return OwnershipKind::Unknown;
  }

  llvm::StringRef name = func->getName();
  if (CppThreadingModel::isDeferLockConstructor(name)) {
    return OwnershipKind::Deferred;
  }
  if (CppThreadingModel::isTryToLockConstructor(name)) {
    return OwnershipKind::Try;
  }
  if (CppThreadingModel::isAdoptLockConstructor(name)) {
    return OwnershipKind::Adopt;
  }
  return OwnershipKind::Immediate;
}

const llvm::AllocaInst *
RAIILockTracker::findLockObjectForConstructor(const llvm::CallBase *ctor) {
  if (!ctor || ctor->getNumOperands() == 0)
    return nullptr;

  // The 'this' pointer is typically the first argument
  llvm::Value *thisPtr = ctor->getArgOperand(0);
  if (!thisPtr)
    return nullptr;

  // Strip casts to find the underlying alloca
  thisPtr = thisPtr->stripPointerCasts();

  // Check if it's an alloca
  if (auto *alloca = llvm::dyn_cast<llvm::AllocaInst>(thisPtr)) {
    return alloca;
  }

  // Sometimes the this pointer comes from a bitcast of an alloca
  for (llvm::User *user : thisPtr->users()) {
    if (auto *alloca = llvm::dyn_cast<llvm::AllocaInst>(user)) {
      return alloca;
    }
  }

  return nullptr;
}

std::vector<const llvm::Value *>
RAIILockTracker::extractUnderlyingLocks(const llvm::CallBase *ctor) {
  std::vector<const llvm::Value *> locks;
  if (!ctor) {
    return locks;
  }

  const auto *func = ctor->getCalledFunction();
  if (!func) {
    func = llvm::dyn_cast<llvm::Function>(
        ctor->getCalledOperand()->stripPointerCasts());
  }
  llvm::StringRef name = func ? func->getName() : llvm::StringRef();
  unsigned last_lock_arg = ctor->arg_size();
  if ((CppThreadingModel::isAdoptLockConstructor(name) ||
       CppThreadingModel::isDeferLockConstructor(name) ||
       CppThreadingModel::isTryToLockConstructor(name)) &&
      last_lock_arg > 2) {
    last_lock_arg = 2;
  }

  for (unsigned idx = 1; idx < last_lock_arg; ++idx) {
    const llvm::Value *lock = ctor->getArgOperand(idx);
    if (lock) {
      locks.push_back(lock);
    }
  }

  return locks;
}

std::vector<const llvm::Instruction *>
RAIILockTracker::findDestructorsForLockObject(
    const llvm::AllocaInst *lockAlloca, const llvm::Function *F) {
  std::vector<const llvm::Instruction *> destructors;
  bool hasExplicitDestructor = false;
  bool hasLifetimeEnd = false;

  if (!lockAlloca || !F)
    return destructors;

  auto addIfMissing = [&](const llvm::Instruction *inst) {
    if (!inst) {
      return;
    }
    if (std::find(destructors.begin(), destructors.end(), inst) ==
        destructors.end()) {
      destructors.push_back(inst);
    }
  };

  // Look for destructor calls that operate on this lock object
  for (llvm::const_inst_iterator I = inst_begin(F), E = inst_end(F); I != E;
       ++I) {
    const llvm::Instruction *inst = &*I;

    if (!isRAIILockDestructor(inst))
      continue;

    const auto *call = llvm::dyn_cast<llvm::CallBase>(inst);
    if (!call)
      continue;

    // Check if this destructor operates on our lock object
    if (call->arg_size() > 0) {
      llvm::Value *thisPtr = call->getArgOperand(0);
      thisPtr = thisPtr->stripPointerCasts();

      if (thisPtr == lockAlloca) {
        hasExplicitDestructor = true;
        addIfMissing(inst);
      }
    }
  }

  // If no explicit destructors found, look for lifetime.end intrinsics
  // or function exits as implicit destructor locations
  if (!hasExplicitDestructor) {
    for (llvm::const_inst_iterator I = inst_begin(F), E = inst_end(F); I != E;
         ++I) {
      const llvm::Instruction *inst = &*I;

      // Check for llvm.lifetime.end
      if (const auto *intrinsic = llvm::dyn_cast<llvm::IntrinsicInst>(inst)) {
        if (intrinsic->getIntrinsicID() == llvm::Intrinsic::lifetime_end) {
          if (intrinsic->getArgOperand(1)->stripPointerCasts() == lockAlloca) {
            hasLifetimeEnd = true;
            addIfMissing(inst);
          }
        }
      }
    }
  }

  return destructors;
}

void RAIILockTracker::processConstructor(const llvm::CallBase *ctor,
                                         const llvm::Function *F) {
  const llvm::AllocaInst *lockObj = findLockObjectForConstructor(ctor);
  if (!lockObj)
    return;

  // Skip if we've already processed this lock object
  if (lockLifetimes.find(lockObj) != lockLifetimes.end())
    return;

  LockLifetime lifetime;
  lifetime.lockObject = lockObj;
  lifetime.constructor = ctor;
  lifetime.underlyingLocks = extractUnderlyingLocks(ctor);
  lifetime.ownership = getOwnershipKind(ctor);
  const llvm::Function *ctorFunc = ctor->getCalledFunction();
  if (!ctorFunc) {
    ctorFunc = llvm::dyn_cast<llvm::Function>(
        ctor->getCalledOperand()->stripPointerCasts());
  }
  lifetime.isScoped = ctorFunc && CppThreadingModel::isScopedLockConstructor(
                                      ctorFunc->getName());
  lifetime.sharedModes.assign(lifetime.underlyingLocks.size(),
                              isSharedLock(ctor));

  // Find all destructor calls for this lock object
  lifetime.destructors = findDestructorsForLockObject(lockObj, F);
  lifetime.hasPreciseLifetimeEnd = !lifetime.destructors.empty();
  if (!lifetime.hasPreciseLifetimeEnd) {
    if (ctor->getParent() != &F->getEntryBlock()) {
      lifetime.impreciseLifetimeBoundary =
          findImpreciseLifetimeBoundary(lockObj, F);
    }
  }

  lockLifetimes[lockObj] = lifetime;
}

void RAIILockTracker::analyzeFunction(const llvm::Function *F) {
  if (!F)
    return;

  // Clear previous state
  lockLifetimes.clear();
  processedDestructors.clear();

  // First pass: find all RAII lock constructors
  for (llvm::const_inst_iterator I = inst_begin(F), E = inst_end(F); I != E;
       ++I) {
    const llvm::Instruction *inst = &*I;

    if (!isRAIILockConstructor(inst))
      continue;

    const auto *call = llvm::dyn_cast<llvm::CallBase>(inst);
    if (call) {
      processConstructor(call, F);
    }
  }
}

const LockLifetime *
RAIILockTracker::getLockLifetime(const llvm::AllocaInst *alloca) const {
  auto it = lockLifetimes.find(alloca);
  if (it != lockLifetimes.end()) {
    return &it->second;
  }
  return nullptr;
}

} // namespace RAIILock
