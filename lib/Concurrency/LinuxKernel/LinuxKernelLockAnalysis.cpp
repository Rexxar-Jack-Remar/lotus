/**
 * @file LinuxKernelLockAnalysis.cpp
 * @brief Linux Kernel Lock Analysis Implementation
 *
 * @author Lotus Analysis Framework
 * @date 2026
 */

#include "Concurrency/LinuxKernel/LinuxKernelLockAnalysis.h"

#include "Concurrency/LinuxKernel/LinuxKernelProcessModel.h"

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <llvm/ADT/STLExtras.h>

using namespace llvm;

namespace kernel {

void LinuxKernelLockAnalysis::analyzeLocks() {
  lock_regions_.clear();
  lock_diagnostics_.clear();
  std::map<const Function *, std::vector<const KernelOperation *>> held_locks;

  for (const auto &op : process_model_.getAllOperations()) {
    if (!isLockAcquire(op.kind) && !isLockRelease(op.kind)) {
      continue;
    }
    if (op.lock == nullptr) {
      continue;
    }

    auto &stack = held_locks[op.inst->getFunction()];
    if (isLockAcquire(op.kind)) {
      stack.push_back(&op);
    } else {
      auto it = llvm::find_if(llvm::reverse(stack), [&](const KernelOperation *held) {
        return held->lock == op.lock;
      });
      if (it != stack.rend()) {
        const KernelOperation *acquire = *it;
        LockRegion region;
        region.acquire_inst = acquire->inst;
        region.release_inst = op.inst;
        region.lock = op.lock;
        region.kind = op.lock_kind;
        region.is_critical = true;
        lock_regions_.push_back(region);
        stack.erase(std::next(it).base());
      }
    }
  }

  for (const auto &pair : process_model_.getLockInfoMap()) {
    const LockInfo &info = pair.second;
    lock_diagnostics_["total_locks"]++;
    if (info.is_recursive) {
      lock_diagnostics_["recursive_locks"]++;
    }
    if (info.is_interruptible) {
      lock_diagnostics_["interruptible_locks"]++;
    }
    if (info.is_raw) {
      lock_diagnostics_["raw_locks"]++;
    }
  }
}

bool LinuxKernelLockAnalysis::isLockAcquire(OperationKind kind) const {
  return kind == OperationKind::LOCK_ACQUIRE || kind == OperationKind::LOCK_TRY;
}

bool LinuxKernelLockAnalysis::isLockRelease(OperationKind kind) const {
  return kind == OperationKind::LOCK_RELEASE;
}

bool LinuxKernelLockAnalysis::formsDeadlock(const LockRegion &r1,
                                            const LockRegion &r2) const {
  if (r1.lock == r2.lock) {
    return false;
  }

  const auto chain1 = getLockChain(r1.acquire_inst);
  const auto chain2 = getLockChain(r2.acquire_inst);
  if (chain1.empty() || chain2.empty()) {
    return false;
  }

  return llvm::is_contained(chain1, r2.lock) && llvm::is_contained(chain2, r1.lock);
}

std::vector<LockID>
LinuxKernelLockAnalysis::getLockChain(const Instruction *inst) const {
  std::vector<LockID> chain;
  if (inst == nullptr) {
    return chain;
  }

  std::map<LockID, int> held_locks;
  for (const KernelOperation *op :
       process_model_.getOperationsInFunction(inst->getFunction())) {
    if (op->inst == inst) {
      break;
    }
    if (op->lock == nullptr) {
      continue;
    }
    if (isLockAcquire(op->kind)) {
      if (held_locks[op->lock]++ == 0) {
        chain.push_back(op->lock);
      }
    } else if (isLockRelease(op->kind) && held_locks[op->lock] > 0) {
      if (--held_locks[op->lock] == 0) {
        chain.erase(std::remove(chain.begin(), chain.end(), op->lock),
                    chain.end());
      }
    }
  }
  return chain;
}

std::vector<std::pair<const Instruction *, const Instruction *>>
LinuxKernelLockAnalysis::findPotentialDeadlocks() const {
  std::vector<std::pair<const Instruction *, const Instruction *>> deadlocks;
  auto inversions = findLockOrderInversions();
  deadlocks.insert(deadlocks.end(), inversions.begin(), inversions.end());
  lock_diagnostics_["deadlock_checks"] += lock_regions_.size();
  return deadlocks;
}

std::vector<const Instruction *>
LinuxKernelLockAnalysis::findDoubleLocks() const {
  return process_model_.findDoubleLocks();
}

std::vector<const Instruction *>
LinuxKernelLockAnalysis::findUnlockWithoutLock() const {
  return process_model_.findUnlockWithoutLock();
}

std::vector<const Instruction *>
LinuxKernelLockAnalysis::findLockWithoutUnlock() const {
  std::vector<const Instruction *> result;

  for (const auto &pair : process_model_.getLockInfoMap()) {
    const LockInfo &info = pair.second;

    if (info.acquire_count > info.release_count) {
      if (info.acquire_inst) {
        result.push_back(info.acquire_inst);
      }
    }
  }

  return result;
}

std::vector<std::pair<const Instruction *, const Instruction *>>
LinuxKernelLockAnalysis::findLockOrderInversions() const {
  std::vector<std::pair<const Instruction *, const Instruction *>> inversions;
  struct LockOrderEvidence {
    const Instruction *first_acquire = nullptr;
    const Instruction *second_acquire = nullptr;
  };

  std::map<std::pair<LockID, LockID>, LockOrderEvidence> orders;

  for (const auto &op : process_model_.getAllOperations()) {
    if (!isLockAcquire(op.kind) || op.lock == nullptr) {
      continue;
    }

    for (LockID held : getLockChain(op.inst)) {
      if (held == nullptr || held == op.lock || held == op.inst) {
        continue;
      }
      auto key = std::make_pair(held, op.lock);
      auto &evidence = orders[key];
      if (evidence.first_acquire == nullptr) {
        evidence.first_acquire = process_model_.getLockInfoMap().at(held).acquire_inst;
        evidence.second_acquire = op.inst;
      }
    }
  }

  for (const auto &entry : orders) {
    auto reverse = std::make_pair(entry.first.second, entry.first.first);
    auto reverse_it = orders.find(reverse);
    if (reverse_it == orders.end()) {
      continue;
    }
    if (entry.first < reverse) {
      inversions.emplace_back(entry.second.second_acquire,
                              reverse_it->second.second_acquire);
    }
  }

  lock_diagnostics_["lock_order_checks"] += orders.size();
  return inversions;
}

std::vector<const Instruction *>
LinuxKernelLockAnalysis::findMixRawAndCookedLocks() const {
  std::vector<const Instruction *> result = process_model_.findMixRawAndcooked();
  llvm::sort(result);
  result.erase(std::unique(result.begin(), result.end()), result.end());
  return result;
}

std::vector<const Instruction *>
LinuxKernelLockAnalysis::findSleepInSpinlock() const {
  std::vector<const Instruction *> result;
  for (const auto &op : process_model_.getAllOperations()) {
    const bool may_sleep =
        op.kind == OperationKind::WAIT_EVENT ||
        op.kind == OperationKind::COMPLETION_WAIT ||
        (op.kind == OperationKind::LOCK_ACQUIRE &&
         (op.lock_kind == LockKind::MUTEX || op.lock_kind == LockKind::SEMAPHORE ||
          op.lock_kind == LockKind::RW_SEMAPHORE));
    if (!may_sleep) {
      continue;
    }

    for (LockID held : getLockChain(op.inst)) {
      auto info_it = process_model_.getLockInfoMap().find(held);
      if (info_it == process_model_.getLockInfoMap().end()) {
        continue;
      }
      if (info_it->second.kind == LockKind::SPINLOCK ||
          info_it->second.kind == LockKind::RWLOCK || info_it->second.is_raw) {
        result.push_back(op.inst);
        break;
      }
    }
  }
  return result;
}

std::vector<const Instruction *>
LinuxKernelLockAnalysis::findIrqSaveRestoreIssues() const {
  return process_model_.findIrqSaveRestoreMismatch();
}

} // namespace kernel
