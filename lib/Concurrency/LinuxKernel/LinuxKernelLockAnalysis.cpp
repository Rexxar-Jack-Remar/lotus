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
#include <deque>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <llvm/ADT/STLExtras.h>
#include <llvm/Analysis/PostDominators.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Instructions.h>

using namespace llvm;

namespace kernel {

namespace {

const Value *stripIntegerCasts(const Value *value) {
  while (const auto *cast = dyn_cast<CastInst>(value)) {
    value = cast->getOperand(0);
  }
  return value;
}

std::optional<bool> trueBranchMeansSuccess(const BranchInst *branch,
                                           const KernelOperation &op) {
  if (branch == nullptr || !branch->isConditional()) {
    return std::nullopt;
  }
  const auto *call = dyn_cast<CallBase>(op.inst);
  if (call == nullptr || call->getType()->isVoidTy()) {
    return std::nullopt;
  }

  const Value *condition = stripIntegerCasts(branch->getCondition());
  bool true_means_nonzero = false;
  if (stripIntegerCasts(condition) == call) {
    true_means_nonzero = true;
  } else if (const auto *cmp = dyn_cast<ICmpInst>(condition)) {
    const Value *lhs = stripIntegerCasts(cmp->getOperand(0));
    const Value *rhs = stripIntegerCasts(cmp->getOperand(1));
    const ConstantInt *constant = dyn_cast<ConstantInt>(rhs);
    if (lhs != call || constant == nullptr || !constant->isZero() ||
        (cmp->getPredicate() != ICmpInst::ICMP_EQ &&
         cmp->getPredicate() != ICmpInst::ICMP_NE)) {
      return std::nullopt;
    }
    true_means_nonzero = cmp->getPredicate() == ICmpInst::ICMP_NE;
  } else {
    return std::nullopt;
  }

  if (op.conditional_success == ConditionalSuccess::NONZERO) {
    return true_means_nonzero;
  }
  if (op.conditional_success == ConditionalSuccess::ZERO) {
    return !true_means_nonzero;
  }
  return std::nullopt;
}

template <typename T>
void appendUnique(std::vector<T> &values, const T &value) {
  if (!llvm::is_contained(values, value)) {
    values.push_back(value);
  }
}

} // namespace

void LinuxKernelLockAnalysis::analyzeLocks() {
  lock_regions_.clear();
  lock_diagnostics_.clear();
  lock_order_inversions_.clear();
  double_locks_.clear();
  unlock_without_lock_.clear();
  lock_without_unlock_.clear();
  sleep_in_spinlock_.clear();

  using OrderKey = std::pair<LockID, LockID>;
  std::map<OrderKey, const Instruction *> order_evidence;
  std::map<std::pair<const Function *, LockID>, const Instruction *>
      acquire_evidence;

  auto applyAcquire = [](LockFlowState &state, const KernelOperation &op,
                         bool definite) {
    state.may_held.insert(op.lock);
    if (definite) {
      state.must_held.insert(op.lock);
    }
    if (op.lock_mode == LockMode::EXCLUSIVE) {
      state.may_exclusive.insert(op.lock);
      if (definite) {
        state.must_exclusive.insert(op.lock);
      }
    }
  };

  auto applyRelease = [](LockFlowState &state, LockID lock) {
    state.may_held.erase(lock);
    state.must_held.erase(lock);
    state.may_exclusive.erase(lock);
    state.must_exclusive.erase(lock);
  };

  auto mergeState = [](LockFlowState &destination,
                       const LockFlowState &incoming) {
    LockFlowState merged = destination;
    merged.may_held.insert(incoming.may_held.begin(), incoming.may_held.end());
    merged.may_exclusive.insert(incoming.may_exclusive.begin(),
                                incoming.may_exclusive.end());

    std::set<LockID> must_held;
    std::set_intersection(destination.must_held.begin(),
                          destination.must_held.end(),
                          incoming.must_held.begin(), incoming.must_held.end(),
                          std::inserter(must_held, must_held.end()));
    merged.must_held = std::move(must_held);

    std::set<LockID> must_exclusive;
    std::set_intersection(
        destination.must_exclusive.begin(), destination.must_exclusive.end(),
        incoming.must_exclusive.begin(), incoming.must_exclusive.end(),
        std::inserter(must_exclusive, must_exclusive.end()));
    merged.must_exclusive = std::move(must_exclusive);
    return merged;
  };

  auto isAtomicLock = [&](LockID lock) {
    auto info = process_model_.getLockInfoMap().find(lock);
    if (info == process_model_.getLockInfoMap().end()) {
      return false;
    }
    if (info->second.is_raw) {
      return true;
    }
    return !process_model_.isPreemptRT() &&
           (info->second.kind == LockKind::SPINLOCK ||
            info->second.kind == LockKind::RWLOCK);
  };

  for (const Function &function : process_model_.getModule()) {
    if (function.isDeclaration()) {
      continue;
    }

    std::map<const BasicBlock *, LockFlowState> in_states;
    std::set<const BasicBlock *> initialized;
    std::deque<const BasicBlock *> worklist;
    const BasicBlock *entry = &function.getEntryBlock();
    initialized.insert(entry);
    worklist.push_back(entry);

    while (!worklist.empty()) {
      const BasicBlock *block = worklist.front();
      worklist.pop_front();
      LockFlowState state = in_states[block];

      struct PendingConditional {
        const KernelOperation *op = nullptr;
        bool was_may_held = false;
        bool was_must_held = false;
        bool was_may_exclusive = false;
        bool was_must_exclusive = false;
      };
      std::vector<PendingConditional> pending;

      for (const Instruction &instruction : *block) {
        const KernelOperation *op =
            process_model_.getOperationForInstruction(&instruction);
        if (op == nullptr || op->lock == nullptr) {
          continue;
        }
        if (isLockAcquire(op->kind)) {
          if (op->conditional_success == ConditionalSuccess::UNCONDITIONAL) {
            applyAcquire(state, *op, true);
          } else {
            pending.push_back({op, state.may_held.count(op->lock) > 0,
                               state.must_held.count(op->lock) > 0,
                               state.may_exclusive.count(op->lock) > 0,
                               state.must_exclusive.count(op->lock) > 0});
            applyAcquire(state, *op, false);
          }
        } else if (isLockRelease(op->kind)) {
          applyRelease(state, op->lock);
        }
      }

      const auto *branch = dyn_cast<BranchInst>(block->getTerminator());
      for (const BasicBlock *successor : successors(block)) {
        LockFlowState edge_state = state;
        if (branch != nullptr && branch->isConditional()) {
          const bool true_edge = successor == branch->getSuccessor(0);
          for (const PendingConditional &conditional : pending) {
            std::optional<bool> true_is_success =
                trueBranchMeansSuccess(branch, *conditional.op);
            if (!true_is_success.has_value()) {
              continue;
            }
            const bool success_edge = true_edge == *true_is_success;
            if (success_edge) {
              applyAcquire(edge_state, *conditional.op, true);
              continue;
            }
            if (!conditional.was_may_held) {
              edge_state.may_held.erase(conditional.op->lock);
            }
            if (!conditional.was_must_held) {
              edge_state.must_held.erase(conditional.op->lock);
            }
            if (!conditional.was_may_exclusive) {
              edge_state.may_exclusive.erase(conditional.op->lock);
            }
            if (!conditional.was_must_exclusive) {
              edge_state.must_exclusive.erase(conditional.op->lock);
            }
          }
        }

        if (initialized.insert(successor).second) {
          in_states[successor] = edge_state;
          worklist.push_back(successor);
          continue;
        }
        LockFlowState merged = mergeState(in_states[successor], edge_state);
        if (merged != in_states[successor]) {
          in_states[successor] = std::move(merged);
          worklist.push_back(successor);
        }
      }
    }

    DominatorTree dominators(*const_cast<Function *>(&function));
    PostDominatorTree post_dominators;
    post_dominators.recalculate(*const_cast<Function *>(&function));
    for (const KernelOperation *acquire :
         process_model_.getOperationsInFunction(&function)) {
      if (!isLockAcquire(acquire->kind) || acquire->lock == nullptr ||
          acquire->conditional_success != ConditionalSuccess::UNCONDITIONAL) {
        continue;
      }
      const KernelOperation *best_release = nullptr;
      for (const KernelOperation *release :
           process_model_.getOperationsInFunction(&function)) {
        if (!isLockRelease(release->kind) || release->lock != acquire->lock ||
            !dominators.dominates(acquire->inst, release->inst) ||
            !post_dominators.dominates(release->inst, acquire->inst)) {
          continue;
        }
        if (best_release == nullptr ||
            dominators.dominates(release->inst, best_release->inst)) {
          best_release = release;
        }
      }
      if (best_release != nullptr) {
        lock_regions_.push_back({acquire->inst, best_release->inst,
                                 acquire->lock, acquire->lock_kind, true});
      }
    }

    // Collect diagnostics only after the fixed point so transient worklist
    // states cannot leak into results.
    for (const BasicBlock &block : function) {
      if (initialized.count(&block) == 0) {
        continue;
      }
      LockFlowState state = in_states[&block];
      for (const Instruction &instruction : block) {
        const KernelOperation *op =
            process_model_.getOperationForInstruction(&instruction);
        if (op == nullptr) {
          continue;
        }

        if (op->lock != nullptr && isLockAcquire(op->kind)) {
          const bool sleeping_acquire =
              op->kind == OperationKind::LOCK_ACQUIRE &&
              (op->lock_kind == LockKind::MUTEX ||
               op->lock_kind == LockKind::SEMAPHORE ||
               op->lock_kind == LockKind::RW_SEMAPHORE);
          if (sleeping_acquire) {
            for (LockID held : state.may_held) {
              if (isAtomicLock(held)) {
                appendUnique(sleep_in_spinlock_, op->inst);
                break;
              }
            }
          }

          const bool current_exclusive = op->lock_mode != LockMode::SHARED;
          const bool incompatible_self =
              state.may_held.count(op->lock) > 0 &&
              (current_exclusive || state.may_exclusive.count(op->lock) > 0);
          if (incompatible_self) {
            appendUnique(double_locks_, op->inst);
          }

          for (LockID held : state.may_held) {
            if (held == op->lock) {
              continue;
            }
            const bool held_exclusive = state.may_exclusive.count(held) > 0;
            if (!current_exclusive && !held_exclusive) {
              continue;
            }
            order_evidence.emplace(std::make_pair(held, op->lock), op->inst);
          }
          acquire_evidence[{&function, op->lock}] = op->inst;
          applyAcquire(state, *op,
                       op->conditional_success ==
                           ConditionalSuccess::UNCONDITIONAL);
          continue;
        }

        if (op->lock != nullptr && isLockRelease(op->kind)) {
          if (state.may_held.count(op->lock) == 0) {
            appendUnique(unlock_without_lock_, op->inst);
          }
          applyRelease(state, op->lock);
          continue;
        }

        const bool may_sleep = op->kind == OperationKind::WAIT_EVENT ||
                               op->kind == OperationKind::COMPLETION_WAIT ||
                               (op->kind == OperationKind::LOCK_ACQUIRE &&
                                (op->lock_kind == LockKind::MUTEX ||
                                 op->lock_kind == LockKind::SEMAPHORE ||
                                 op->lock_kind == LockKind::RW_SEMAPHORE));
        if (!may_sleep) {
          continue;
        }
        for (LockID held : state.may_held) {
          if (isAtomicLock(held)) {
            appendUnique(sleep_in_spinlock_, op->inst);
            break;
          }
        }
      }

      if (isa<ReturnInst>(block.getTerminator())) {
        for (LockID held : state.may_held) {
          auto evidence = acquire_evidence.find({&function, held});
          if (evidence != acquire_evidence.end()) {
            appendUnique(lock_without_unlock_, evidence->second);
          }
        }
      }
    }
  }

  for (const auto &entry : order_evidence) {
    OrderKey reverse{entry.first.second, entry.first.first};
    auto reverse_it = order_evidence.find(reverse);
    if (reverse_it != order_evidence.end() && entry.first < reverse) {
      lock_order_inversions_.emplace_back(entry.second, reverse_it->second);
    }
  }

  for (const auto &pair : process_model_.getLockInfoMap()) {
    const LockInfo &info = pair.second;
    lock_diagnostics_["total_locks"]++;
    if (info.is_recursive) {
      lock_diagnostics_["recursive_locks"]++;
    }
    if (info.is_nested) {
      lock_diagnostics_["nested_locks"]++;
    }
    if (info.is_interruptible) {
      lock_diagnostics_["interruptible_locks"]++;
    }
    if (info.is_raw) {
      lock_diagnostics_["raw_locks"]++;
    }
  }
  lock_diagnostics_["lock_order_checks"] = order_evidence.size();
  lock_diagnostics_["deadlock_checks"] = 0;
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

  return llvm::is_contained(chain1, r2.lock) &&
         llvm::is_contained(chain2, r1.lock);
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
  // A lock-order inversion is not by itself a feasible deadlock.  Promotion
  // requires execution-context reachability/MHP information that this pass
  // does not yet have.
  return {};
}

std::vector<const Instruction *>
LinuxKernelLockAnalysis::findDoubleLocks() const {
  return double_locks_;
}

std::vector<const Instruction *>
LinuxKernelLockAnalysis::findUnlockWithoutLock() const {
  return unlock_without_lock_;
}

std::vector<const Instruction *>
LinuxKernelLockAnalysis::findLockWithoutUnlock() const {
  return lock_without_unlock_;
}

std::vector<std::pair<const Instruction *, const Instruction *>>
LinuxKernelLockAnalysis::findLockOrderInversions() const {
  return lock_order_inversions_;
}

std::vector<const Instruction *>
LinuxKernelLockAnalysis::findMixRawAndCookedLocks() const {
  std::vector<const Instruction *> result =
      process_model_.findMixRawAndcooked();
  llvm::sort(result);
  result.erase(std::unique(result.begin(), result.end()), result.end());
  return result;
}

std::vector<const Instruction *>
LinuxKernelLockAnalysis::findSleepInSpinlock() const {
  return sleep_in_spinlock_;
}

std::vector<const Instruction *>
LinuxKernelLockAnalysis::findIrqSaveRestoreIssues() const {
  return process_model_.findIrqSaveRestoreMismatch();
}

} // namespace kernel
