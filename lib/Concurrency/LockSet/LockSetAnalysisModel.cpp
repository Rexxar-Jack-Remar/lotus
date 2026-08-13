#include "Alias/Infrastructure/AliasAnalysisWrapper/AliasAnalysisWrapper.h"
#include "Concurrency/LockSet/LockSetAnalysis.h"
#include "Concurrency/LockSet/LockSetAnalysisSupport.h"

#include <algorithm>
#include <queue>
#include <set>

#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>

using namespace llvm;
using namespace mhp;

void LockSetAnalysis::identifyLocks() {
  auto process_func = [this](Function &func) {
    for (inst_iterator I = inst_begin(func), E = inst_end(func); I != E; ++I) {
      Instruction *inst = &*I;
      const ThreadAPI::LockSemantics lock_semantics =
          m_thread_api->describeLockSemantics(inst);

      if (detail::isNonBinarySemaphoreOp(m_thread_api, inst)) {
        continue;
      }

      std::vector<LockID> raii_releases = getRAIILocksReleasedAt(inst);
      for (LockID lock : raii_releases) {
        if (!lock) {
          continue;
        }
        m_all_locks.insert(lock);
        m_lock_releases[lock].push_back(inst);
      }
      if (!raii_releases.empty()) {
        continue;
      }

      const bool is_acquire = lock_semantics.is_acquire;
      const bool is_release = lock_semantics.is_release;

      if (is_acquire) {
        LockID lock = getLockValue(inst);
        if (!lock) {
          lock = getCppWrapperLockValue(inst);
        }
        if (lock) {
          m_all_locks.insert(lock);
          m_lock_acquires[lock].push_back(inst);

          LockSet entry_locks = getMayLockSetAt(inst);
          for (const auto *held : entry_locks) {
            if (held == lock || mayAlias(held, lock)) {
              m_reentrant_locks.insert(getCanonicalLock(lock));
              break;
            }
          }

          if (m_thread_api->isTryLock(inst))
            m_lock_try_acquires[lock].push_back(inst);
        }
      } else if (is_release) {
        LockID lock = getLockValue(inst);
        if (!lock) {
          lock = getCppWrapperLockValue(inst);
        }
        if (lock) {
          m_all_locks.insert(lock);
          m_lock_releases[lock].push_back(inst);
        }
      }
    }
  };

  if (m_module) {
    for (Function &func : *m_module) {
      if (!func.isDeclaration()) {
        process_func(func);
      }
    }
  } else if (m_single_function) {
    process_func(*m_single_function);
  }
}

void LockSetAnalysis::trackLockOrdering() {
  for (const auto &pair : m_may_locksets_entry) {
    const Instruction *inst = pair.first;
    const LockSet &locks_held = pair.second;
    auto exit_it = m_may_locksets_exit.find(inst);
    if (exit_it == m_may_locksets_exit.end()) {
      continue;
    }

    LockSet newly_acquired;
    std::set_difference(exit_it->second.begin(), exit_it->second.end(),
                        locks_held.begin(), locks_held.end(),
                        std::inserter(newly_acquired, newly_acquired.begin()));

    const LockSet held_read = getMayReadLockSetAt(inst);
    const LockSet held_write = getMayWriteLockSetAt(inst);
    LockSet newly_acquired_read;
    LockSet newly_acquired_write;
    auto read_exit_it = m_may_read_locks_exit.find(inst);
    if (read_exit_it != m_may_read_locks_exit.end()) {
      std::set_difference(
          read_exit_it->second.begin(), read_exit_it->second.end(),
          held_read.begin(), held_read.end(),
          std::inserter(newly_acquired_read, newly_acquired_read.begin()));
    }
    auto write_exit_it = m_may_write_locks_exit.find(inst);
    if (write_exit_it != m_may_write_locks_exit.end()) {
      std::set_difference(
          write_exit_it->second.begin(), write_exit_it->second.end(),
          held_write.begin(), held_write.end(),
          std::inserter(newly_acquired_write, newly_acquired_write.begin()));
    }
    const bool nonblocking = m_thread_api->isTryLock(inst);

    auto markIfReentrantAcquire = [&](LockID acquired_lock) {
      if (!acquired_lock) {
        return;
      }
      for (const auto *held_lock : locks_held) {
        if (held_lock == acquired_lock || mayAlias(held_lock, acquired_lock)) {
          m_reentrant_locks.insert(getCanonicalLock(acquired_lock));
          return;
        }
      }
    };

    if (isLockOperation(inst)) {
      if (LockID op_lock = getLockValue(inst)) {
        const auto *call = dyn_cast<CallBase>(inst);
        const ThreadAPI::TD_TYPE type =
            call ? m_thread_api->getType(call) : ThreadAPI::TD_DUMMY;
        if (type != ThreadAPI::TD_SHARED_RDLOCK &&
            type != ThreadAPI::TD_SHARED_WRLOCK &&
            type != ThreadAPI::TD_SHARED_LOCK_CTOR &&
            type != ThreadAPI::TD_SHARED_UNLOCK) {
          markIfReentrantAcquire(op_lock);
        }
      }
    }

    for (LockID new_lock : newly_acquired) {
      markIfReentrantAcquire(new_lock);

      for (const auto *held_lock : locks_held) {
        if (held_lock != new_lock) {
          m_observed_lock_orders.insert({held_lock, new_lock});
          const bool held_only_shared = held_read.count(held_lock) != 0 &&
                                        held_write.count(held_lock) == 0;
          const bool request_only_shared =
              newly_acquired_read.count(new_lock) != 0 &&
              newly_acquired_write.count(new_lock) == 0;
          if (!nonblocking && !(held_only_shared && request_only_shared)) {
            m_deadlock_wait_orders.insert({held_lock, new_lock});
          }
        }
      }
    }
  }
}

bool LockSetAnalysis::mayAlias(LockID lock1, LockID lock2) const {
  const Module *module =
      m_module ? m_module
               : (m_single_function ? m_single_function->getParent() : nullptr);
  if (detail::areDisjointConstantOffsetPointers(lock1, lock2, module)) {
    return false;
  }

  auto getKnownDistinctObject = [](LockID lock) -> const Value * {
    if (!lock) {
      return nullptr;
    }
    lock = lock->stripPointerCasts();
    const Value *object = getUnderlyingObject(lock, 32);
    object = object ? object->stripPointerCasts() : lock;
    return isa<GlobalVariable>(object) || isa<AllocaInst>(object) ? object
                                                                  : nullptr;
  };
  const Value *object1 = getKnownDistinctObject(lock1);
  const Value *object2 = getKnownDistinctObject(lock2);
  if (object1 && object2 && object1 != object2) {
    return false;
  }

  lock1 = getCanonicalLock(lock1);
  lock2 = getCanonicalLock(lock2);
  if (lock1 == lock2)
    return true;

  if (m_alias_analysis && lock1 && lock2) {
    return m_alias_analysis->mayAlias(lock1, lock2);
  }

  return true;
}

LockID LockSetAnalysis::getCanonicalLock(LockID lock) const {
  if (!lock)
    return nullptr;

  lock = lock->stripPointerCasts();

  if (const auto *LI = dyn_cast<LoadInst>(lock)) {
    const Value *addr = LI->getPointerOperand()->stripPointerCasts();
    if (const auto *GEP = dyn_cast<GetElementPtrInst>(addr)) {
      if (GEP->hasAllConstantIndices()) {
        return addr;
      }
      lock = GEP->getPointerOperand()->stripPointerCasts();
    } else if (const Value *base = getUnderlyingObject(addr, 32)) {
      lock = base->stripPointerCasts();
    } else {
      lock = addr;
    }
  }

  if (const auto *GEP = dyn_cast<GetElementPtrInst>(lock)) {
    if (GEP->hasAllConstantIndices()) {
      return lock;
    }
    lock = GEP->getPointerOperand()->stripPointerCasts();
  } else if (const Value *base = getUnderlyingObject(lock, 32)) {
    lock = base->stripPointerCasts();
  }

  if (m_alias_analysis) {
    std::vector<const Value *> pts;
    if (m_alias_analysis->getPointsToSet(lock, pts) && pts.size() == 1 &&
        pts.front()) {
      return pts.front()->stripPointerCasts();
    }
  }

  return lock;
}

LockID LockSetAnalysis::getUnderlyingRAIILock(const Instruction *inst,
                                              const Value *lock_obj) const {
  std::vector<LockID> locks = getUnderlyingRAIILocks(inst, lock_obj);
  return locks.empty() ? nullptr : locks.front();
}

std::vector<LockID>
LockSetAnalysis::getUnderlyingRAIILocks(const Instruction *inst,
                                        const Value *lock_obj) const {
  std::vector<LockID> locks;
  if (!inst || !lock_obj) {
    return locks;
  }

  const Function *parent_func = inst->getFunction();
  auto raii_it = m_raii_locks.find(parent_func);
  if (raii_it == m_raii_locks.end()) {
    return locks;
  }

  const Value *base = lock_obj->stripPointerCasts();
  if (const Value *underlying = getUnderlyingObject(base, 32)) {
    base = underlying->stripPointerCasts();
  }

  auto lifetime_range = raii_it->second.equal_range(base);
  if (lifetime_range.first == lifetime_range.second) {
    return locks;
  }

  std::vector<const RAIILock::LockLifetime *> selected_lifetimes;
  for (auto lifetime_it = lifetime_range.first;
       lifetime_it != lifetime_range.second; ++lifetime_it) {
    const RAIILock::LockLifetime &lifetime = lifetime_it->second;
    if (std::find(lifetime.destructors.begin(), lifetime.destructors.end(),
                  inst) != lifetime.destructors.end()) {
      selected_lifetimes.push_back(&lifetime);
      continue;
    }

    if (!lifetime.constructor ||
        lifetime.constructor->getFunction() != inst->getFunction()) {
      continue;
    }
    if (RAIILock::RAIILockTracker::constructorCanReachWithoutReconstruction(
            lifetime.constructor, inst, lifetime.lockObject)) {
      selected_lifetimes.push_back(&lifetime);
    }
  }

  if (selected_lifetimes.empty()) {
    return locks;
  }

  for (const RAIILock::LockLifetime *lifetime : selected_lifetimes) {
    for (const Value *lock : lifetime->underlyingLocks) {
      if (LockID canonical = getCanonicalLock(lock)) {
        if (std::find(locks.begin(), locks.end(), canonical) == locks.end()) {
          locks.push_back(canonical);
        }
      }
    }
  }
  return locks;
}

std::vector<LockID>
LockSetAnalysis::getRAIILocksReleasedAt(const Instruction *inst) const {
  std::vector<LockID> locks;
  if (!inst) {
    return locks;
  }

  const Function *parent_func = inst->getFunction();
  auto raii_it = m_raii_locks.find(parent_func);
  if (raii_it == m_raii_locks.end()) {
    return locks;
  }

  for (const auto &entry : raii_it->second) {
    const RAIILock::LockLifetime &lifetime = entry.second;
    if (std::find(lifetime.destructors.begin(), lifetime.destructors.end(),
                  inst) == lifetime.destructors.end()) {
      continue;
    }

    for (const Value *lock : lifetime.underlyingLocks) {
      if (LockID canonical = getCanonicalLock(lock)) {
        locks.push_back(canonical);
      }
    }
  }

  return locks;
}

std::vector<LockID>
LockSetAnalysis::getImpreciseRAIILocksEndingAt(const Instruction *inst) const {
  std::vector<LockID> locks;
  if (!inst) {
    return locks;
  }

  const Function *parent_func = inst->getFunction();
  auto raii_it = m_raii_locks.find(parent_func);
  if (raii_it == m_raii_locks.end()) {
    return locks;
  }

  for (const auto &entry : raii_it->second) {
    const RAIILock::LockLifetime &lifetime = entry.second;
    if (lifetime.hasPreciseLifetimeEnd) {
      continue;
    }
    const bool at_unknown_boundary = lifetime.impreciseLifetimeBoundary &&
                                     lifetime.impreciseLifetimeBoundary == inst;
    const bool at_function_exit_without_precise_lifetime =
        !lifetime.impreciseLifetimeBoundary &&
        (isa<ReturnInst>(inst) || isa<ResumeInst>(inst));
    if (!at_unknown_boundary && !at_function_exit_without_precise_lifetime) {
      continue;
    }
    for (const Value *lock : lifetime.underlyingLocks) {
      if (LockID canonical = getCanonicalLock(lock)) {
        locks.push_back(canonical);
      }
    }
  }

  return locks;
}

std::vector<LockID>
LockSetAnalysis::getImpreciseRAIILocksInFunction(const Function *func) const {
  std::vector<LockID> locks;
  if (!func) {
    return locks;
  }

  auto raii_it = m_raii_locks.find(func);
  if (raii_it == m_raii_locks.end()) {
    return locks;
  }

  for (const auto &entry : raii_it->second) {
    const RAIILock::LockLifetime &lifetime = entry.second;
    if (lifetime.hasPreciseLifetimeEnd) {
      continue;
    }
    for (const Value *lock : lifetime.underlyingLocks) {
      if (LockID canonical = getCanonicalLock(lock)) {
        locks.push_back(canonical);
      }
    }
  }

  return locks;
}

LockID LockSetAnalysis::getCppWrapperLockValue(const Instruction *inst) const {
  const auto *call = dyn_cast<CallBase>(inst);
  if (!call) {
    return nullptr;
  }

  switch (m_thread_api->getType(call)) {
  case ThreadAPI::TD_SHARED_RDLOCK:
  case ThreadAPI::TD_SHARED_WRLOCK:
  case ThreadAPI::TD_SHARED_UNLOCK:
    if (call->arg_size() >= 1) {
      return getCanonicalLock(call->getArgOperand(0));
    }
    return nullptr;

  case ThreadAPI::TD_LOCK_GUARD_CTOR:
  case ThreadAPI::TD_UNIQUE_LOCK_CTOR:
  case ThreadAPI::TD_SCOPED_LOCK_CTOR:
  case ThreadAPI::TD_SHARED_LOCK_CTOR:
    if (call->arg_size() >= 1) {
      if (LockID tracked =
              getUnderlyingRAIILock(inst, call->getArgOperand(0))) {
        return tracked;
      }
    }
    if (call->arg_size() >= 2) {
      return getCanonicalLock(call->getArgOperand(1));
    }
    return nullptr;

  case ThreadAPI::TD_LOCK_GUARD_DTOR:
  case ThreadAPI::TD_UNIQUE_LOCK_DTOR:
  case ThreadAPI::TD_SCOPED_LOCK_DTOR:
  case ThreadAPI::TD_SHARED_LOCK_DTOR:
  case ThreadAPI::TD_UNIQUE_LOCK_LOCK:
  case ThreadAPI::TD_UNIQUE_LOCK_UNLOCK:
    if (call->arg_size() >= 1) {
      return getUnderlyingRAIILock(inst, call->getArgOperand(0));
    }
    return nullptr;

  default:
    return nullptr;
  }
}

bool LockSetAnalysis::isLockOperation(const Instruction *inst) const {
  if (detail::isNonBinarySemaphoreOp(m_thread_api, inst)) {
    return false;
  }
  if (m_thread_api->isTDAcquire(inst) || m_thread_api->isTDRelease(inst)) {
    return true;
  }

  const auto *call = dyn_cast<CallBase>(inst);
  if (!call) {
    return false;
  }

  switch (m_thread_api->getType(call)) {
  case ThreadAPI::TD_SHARED_RDLOCK:
  case ThreadAPI::TD_SHARED_WRLOCK:
  case ThreadAPI::TD_SHARED_UNLOCK:
  case ThreadAPI::TD_LOCK_GUARD_CTOR:
  case ThreadAPI::TD_LOCK_GUARD_DTOR:
  case ThreadAPI::TD_UNIQUE_LOCK_CTOR:
  case ThreadAPI::TD_UNIQUE_LOCK_DTOR:
  case ThreadAPI::TD_UNIQUE_LOCK_LOCK:
  case ThreadAPI::TD_UNIQUE_LOCK_UNLOCK:
  case ThreadAPI::TD_SCOPED_LOCK_CTOR:
  case ThreadAPI::TD_SCOPED_LOCK_DTOR:
  case ThreadAPI::TD_SHARED_LOCK_CTOR:
  case ThreadAPI::TD_SHARED_LOCK_DTOR:
    return true;
  default:
    return false;
  }
}

LockID LockSetAnalysis::getLockValue(const Instruction *inst) const {
  if (m_thread_api->isTDAcquire(inst) || m_thread_api->isTDRelease(inst)) {
    const auto *call = dyn_cast<CallBase>(inst);
    ThreadAPI::TD_TYPE type =
        call ? m_thread_api->getType(call) : ThreadAPI::TD_DUMMY;
    switch (type) {
    case ThreadAPI::TD_LOCK_GUARD_CTOR:
    case ThreadAPI::TD_LOCK_GUARD_DTOR:
    case ThreadAPI::TD_UNIQUE_LOCK_CTOR:
    case ThreadAPI::TD_UNIQUE_LOCK_DTOR:
    case ThreadAPI::TD_UNIQUE_LOCK_LOCK:
    case ThreadAPI::TD_UNIQUE_LOCK_UNLOCK:
    case ThreadAPI::TD_SCOPED_LOCK_CTOR:
    case ThreadAPI::TD_SCOPED_LOCK_DTOR:
    case ThreadAPI::TD_SHARED_LOCK_CTOR:
    case ThreadAPI::TD_SHARED_LOCK_DTOR:
      return getCanonicalLock(m_thread_api->getLockVal(inst));
    default:
      if (const Value *identity = m_thread_api->getAnalysisLockIdentity(inst)) {
        return getCanonicalLock(identity);
      }
      return getCanonicalLock(m_thread_api->getLockVal(inst));
    }
  }
  return getCppWrapperLockValue(inst);
}
