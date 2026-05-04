#include "Concurrency/LockSet/LockSetAnalysis.h"

#include "Alias/Infrastructure/AliasAnalysisWrapper/AliasAnalysisWrapper.h"
#include "Concurrency/LockSet/LockSetAnalysisInternal.h"

#include <queue>
#include <set>

#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>

using namespace llvm;
using namespace mhp;

void LockSetAnalysis::analyzeFunction(Function *func) {
  if (!func || func->isDeclaration())
    return;

  RAIILock::RAIILockTracker raii_tracker;
  raii_tracker.analyzeFunction(func);
  m_raii_locks[func] = raii_tracker.getAllLockLifetimes();

  detectTryLockSuccessBranches(func);
  computeIntraproceduralLockSets(func);
}

void LockSetAnalysis::detectTryLockSuccessBranches(Function *func) {
  if (!func || func->isDeclaration())
    return;

  for (BasicBlock &bb : *func) {
    auto *br = dyn_cast<BranchInst>(bb.getTerminator());
    if (!br || !br->isConditional())
      continue;

    // Match: %cmp = icmp eq/ne %trylock_ret, 0
    auto *cmp = dyn_cast<ICmpInst>(br->getCondition());
    if (!cmp)
      continue;

    const Value *lhs = cmp->getOperand(0);
    const Value *rhs = cmp->getOperand(1);

    const ConstantInt *zero_const = dyn_cast<ConstantInt>(rhs);
    const Value *trylock_ret = lhs;
    if (!zero_const) {
      zero_const = dyn_cast<ConstantInt>(lhs);
      trylock_ret = rhs;
    }
    if (!zero_const || !zero_const->isZero())
      continue;

    const auto *trylock_call = dyn_cast<CallBase>(trylock_ret);
    if (!trylock_call || !m_thread_api->isTryLock(trylock_call))
      continue;

    LockID lock = getLockValue(trylock_call);
    if (!lock)
      continue;

    // Determine success convention from the TD_TYPE:
    // - pthread_mutex_trylock (TD_TRY_ACQUIRE): returns 0 on success
    // - kernel spin_trylock (TD_KERNEL_SPIN_TRYLOCK): returns non-zero on success
    // - kernel mutex_trylock (TD_KERNEL_MUTEX_TRYLOCK): returns non-zero on
    // success
    // - semaphore try_acquire (TD_SEMAPHORE_TRY_ACQUIRE): returns 0 on success
    ThreadAPI::TD_TYPE type = m_thread_api->getType(trylock_call);
    bool zero_means_success = (type == ThreadAPI::TD_TRY_ACQUIRE ||
                               type == ThreadAPI::TD_SEMAPHORE_TRY_ACQUIRE);

    // For "icmp eq ret, 0": true-branch means ret==0
    // For "icmp ne ret, 0": true-branch means ret!=0
    bool true_branch_is_zero = (cmp->getPredicate() == ICmpInst::ICMP_EQ);
    bool true_branch_is_success =
        (true_branch_is_zero == zero_means_success);

    BasicBlock *success_bb =
        true_branch_is_success ? br->getSuccessor(0) : br->getSuccessor(1);
    const Instruction *first = &success_bb->front();
    m_trylock_success_must_inject[first].insert(lock);
  }
}

void LockSetAnalysis::computeIntraproceduralLockSets(Function *func) {
  auto clearFunctionFacts = [&](Function *target) {
    if (!target) {
      return;
    }
    for (Instruction &inst : instructions(target)) {
      const Instruction *key = &inst;
      m_may_locksets_entry.erase(key);
      m_may_locksets_exit.erase(key);
      m_must_locksets_entry.erase(key);
      m_must_locksets_exit.erase(key);
      m_may_read_locks_entry.erase(key);
      m_may_read_locks_exit.erase(key);
      m_may_write_locks_entry.erase(key);
      m_may_write_locks_exit.erase(key);
      m_must_read_locks_entry.erase(key);
      m_must_read_locks_exit.erase(key);
      m_must_write_locks_entry.erase(key);
      m_must_write_locks_exit.erase(key);
      m_invoke_normal_must_exit.erase(key);
    }
  };

  clearFunctionFacts(func);
  const LockSet all_locks_in_function = getAllLocksInFunction(func);

  std::queue<const Instruction *> worklist;
  std::set<const Instruction *> in_worklist;

  const Instruction *entry = &func->getEntryBlock().front();
  worklist.push(entry);
  in_worklist.insert(entry);

  while (!worklist.empty()) {
    const Instruction *inst = worklist.front();
    worklist.pop();
    in_worklist.erase(inst);

    std::vector<LockSet> may_inputs, must_inputs;
    std::vector<LockSet> may_read_inputs, may_write_inputs;
    std::vector<LockSet> must_read_inputs, must_write_inputs;

    if (inst == entry) {
      may_inputs.push_back(LockSet());
      must_inputs.push_back(LockSet());
      may_read_inputs.push_back(LockSet());
      may_write_inputs.push_back(LockSet());
      must_read_inputs.push_back(LockSet());
      must_write_inputs.push_back(LockSet());
    } else {
      const BasicBlock *bb = inst->getParent();
      if (inst == &bb->front()) {
        for (const BasicBlock *pred : predecessors(bb)) {
          const Instruction *pred_term = pred->getTerminator();
          if (!pred_term) {
            continue;
          }

          // For invoke instructions, use normal-path must facts when this
          // block is the normal destination (not the unwind destination).
          bool use_normal_must = false;
          if (const auto *invoke = dyn_cast<InvokeInst>(pred_term)) {
            if (invoke->getNormalDest() == bb) {
              use_normal_must = true;
            }
          }

          auto it_may = m_may_locksets_exit.find(pred_term);
          auto it_must = m_must_locksets_exit.find(pred_term);
          auto it_mr = m_may_read_locks_exit.find(pred_term);
          auto it_mw = m_may_write_locks_exit.find(pred_term);
          auto it_ur = m_must_read_locks_exit.find(pred_term);
          auto it_uw = m_must_write_locks_exit.find(pred_term);

          // Override must facts with normal-path facts for invoke normal dest
          if (use_normal_must) {
            auto normal_it = m_invoke_normal_must_exit.find(pred_term);
            if (normal_it != m_invoke_normal_must_exit.end()) {
              const auto &nf = normal_it->second;
              if (it_may != m_may_locksets_exit.end()) {
                may_inputs.push_back(it_may->second);
                must_inputs.push_back(nf.must_lockset);
                may_read_inputs.push_back(
                    it_mr != m_may_read_locks_exit.end() ? it_mr->second
                                                         : LockSet());
                may_write_inputs.push_back(
                    it_mw != m_may_write_locks_exit.end() ? it_mw->second
                                                          : LockSet());
                must_read_inputs.push_back(nf.must_read_lockset);
                must_write_inputs.push_back(nf.must_write_lockset);
              } else {
                must_inputs.push_back(nf.must_lockset);
                must_read_inputs.push_back(nf.must_read_lockset);
                must_write_inputs.push_back(nf.must_write_lockset);
              }
              continue;
            }
          }

          if (it_may != m_may_locksets_exit.end()) {
            may_inputs.push_back(it_may->second);
            must_inputs.push_back(it_must != m_must_locksets_exit.end()
                                      ? it_must->second
                                      : all_locks_in_function);
            may_read_inputs.push_back(it_mr != m_may_read_locks_exit.end()
                                          ? it_mr->second
                                          : LockSet());
            may_write_inputs.push_back(it_mw != m_may_write_locks_exit.end()
                                           ? it_mw->second
                                           : LockSet());
            must_read_inputs.push_back(it_ur != m_must_read_locks_exit.end()
                                           ? it_ur->second
                                           : all_locks_in_function);
            must_write_inputs.push_back(it_uw != m_must_write_locks_exit.end()
                                            ? it_uw->second
                                            : all_locks_in_function);
          } else {
            must_inputs.push_back(all_locks_in_function);
            must_read_inputs.push_back(all_locks_in_function);
            must_write_inputs.push_back(all_locks_in_function);
          }
        }
      } else if (const Instruction *prev = inst->getPrevNode()) {
        auto it_may = m_may_locksets_exit.find(prev);
        if (it_may != m_may_locksets_exit.end()) {
          may_inputs.push_back(it_may->second);
          auto it_must = m_must_locksets_exit.find(prev);
          must_inputs.push_back(it_must != m_must_locksets_exit.end()
                                    ? it_must->second
                                    : all_locks_in_function);
          auto it_mr = m_may_read_locks_exit.find(prev);
          may_read_inputs.push_back(it_mr != m_may_read_locks_exit.end()
                                        ? it_mr->second
                                        : LockSet());
          auto it_mw = m_may_write_locks_exit.find(prev);
          may_write_inputs.push_back(it_mw != m_may_write_locks_exit.end()
                                         ? it_mw->second
                                         : LockSet());
          auto it_ur = m_must_read_locks_exit.find(prev);
          must_read_inputs.push_back(it_ur != m_must_read_locks_exit.end()
                                         ? it_ur->second
                                         : all_locks_in_function);
          auto it_uw = m_must_write_locks_exit.find(prev);
          must_write_inputs.push_back(it_uw != m_must_write_locks_exit.end()
                                          ? it_uw->second
                                          : all_locks_in_function);
        }
      }
    }

    LockSet may_read_in =
        may_read_inputs.empty() ? LockSet() : merge(may_read_inputs, false);
    LockSet may_write_in =
        may_write_inputs.empty() ? LockSet() : merge(may_write_inputs, false);
    LockSet must_read_in =
        must_read_inputs.empty() ? LockSet() : merge(must_read_inputs, true);
    LockSet must_write_in =
        must_write_inputs.empty() ? LockSet() : merge(must_write_inputs, true);
    LockSet may_in = may_read_in;
    may_in.insert(may_write_in.begin(), may_write_in.end());
    LockSet must_in = must_read_in;
    must_in.insert(must_write_in.begin(), must_write_in.end());

    // Inject try-lock success must-set overrides
    auto trylock_it = m_trylock_success_must_inject.find(inst);
    if (trylock_it != m_trylock_success_must_inject.end()) {
      for (LockID lock : trylock_it->second) {
        must_in.insert(lock);
        must_write_in.insert(lock);
        may_in.insert(lock);
        may_write_in.insert(lock);
      }
    }

    LockSet may_read_out, may_write_out, must_read_out, must_write_out;
    transferReadWrite(inst, may_read_in, may_write_in, may_read_out,
                      may_write_out, false);
    transferReadWrite(inst, must_read_in, must_write_in, must_read_out,
                      must_write_out, true);

    LockSet may_out = transfer(inst, may_in, false);
    LockSet must_out = transfer(inst, must_in, true);

    if (const auto *invoke = dyn_cast<InvokeInst>(inst)) {
      if (!invoke->doesNotThrow()) {
        // Save the pre-clearing must facts for the normal destination.
        // The cleared facts will flow to the unwind destination.
        InvokeNormalMustFacts normal_facts;
        normal_facts.must_lockset = must_out;
        normal_facts.must_read_lockset = must_read_out;
        normal_facts.must_write_lockset = must_write_out;
        m_invoke_normal_must_exit[inst] = std::move(normal_facts);

        must_read_out.clear();
        must_write_out.clear();
        must_out.clear();
      }
    }

    bool had_entry = m_may_locksets_entry.count(inst);
    bool changed = !had_entry;
    if (m_may_read_locks_entry[inst] != may_read_in) {
      m_may_read_locks_entry[inst] = may_read_in;
      changed = true;
    }
    if (m_may_read_locks_exit[inst] != may_read_out) {
      m_may_read_locks_exit[inst] = may_read_out;
      changed = true;
    }
    if (m_may_write_locks_entry[inst] != may_write_in) {
      m_may_write_locks_entry[inst] = may_write_in;
      changed = true;
    }
    if (m_may_write_locks_exit[inst] != may_write_out) {
      m_may_write_locks_exit[inst] = may_write_out;
      changed = true;
    }
    if (m_must_read_locks_entry[inst] != must_read_in) {
      m_must_read_locks_entry[inst] = must_read_in;
      changed = true;
    }
    if (m_must_read_locks_exit[inst] != must_read_out) {
      m_must_read_locks_exit[inst] = must_read_out;
      changed = true;
    }
    if (m_must_write_locks_entry[inst] != must_write_in) {
      m_must_write_locks_entry[inst] = must_write_in;
      changed = true;
    }
    if (m_must_write_locks_exit[inst] != must_write_out) {
      m_must_write_locks_exit[inst] = must_write_out;
      changed = true;
    }
    if (m_may_locksets_entry[inst] != may_in) {
      m_may_locksets_entry[inst] = may_in;
      changed = true;
    }
    if (m_may_locksets_exit[inst] != may_out) {
      m_may_locksets_exit[inst] = may_out;
      changed = true;
    }
    if (m_must_locksets_entry[inst] != must_in) {
      m_must_locksets_entry[inst] = must_in;
      changed = true;
    }
    if (m_must_locksets_exit[inst] != must_out) {
      m_must_locksets_exit[inst] = must_out;
      changed = true;
    }

    if (!changed) {
      continue;
    }
    if (const Instruction *next = inst->getNextNode()) {
      if (in_worklist.find(next) == in_worklist.end()) {
        worklist.push(next);
        in_worklist.insert(next);
      }
      continue;
    }
    if (!inst->isTerminator()) {
      continue;
    }
    for (const BasicBlock *succ_bb : successors(inst->getParent())) {
      const Instruction *succ = &succ_bb->front();
      if (in_worklist.find(succ) == in_worklist.end()) {
        worklist.push(succ);
        in_worklist.insert(succ);
      }
    }
  }
}

LockSet LockSetAnalysis::transfer(const Instruction *inst,
                                  const LockSet &in_set, bool is_must) const {
  LockSet out_set = in_set;
  auto eraseReleasedLocks = [&](const std::vector<LockID> &locks) {
    for (LockID lock : locks) {
      out_set.erase(lock);
      if (is_must && m_alias_analysis) {
        LockSet to_remove;
        for (const auto *held : out_set) {
          if (mayAlias(held, lock)) {
            to_remove.insert(held);
          }
        }
        for (const auto *held : to_remove) {
          out_set.erase(held);
        }
      }
    }
  };

  std::vector<LockID> raii_releases = getRAIILocksReleasedAt(inst);
  if (!raii_releases.empty()) {
    eraseReleasedLocks(raii_releases);
    return out_set;
  }
  if (is_must) {
    eraseReleasedLocks(getImpreciseRAIILocksEndingAt(inst));
  }

  const CallBase *call = dyn_cast<CallBase>(inst);
  ThreadAPI::TD_TYPE call_type =
      call ? m_thread_api->getType(call) : ThreadAPI::TD_DUMMY;
  if (detail::isNonBinarySemaphoreOp(m_thread_api, inst)) {
    return out_set;
  }
  const bool raw_lock_api = call_type == ThreadAPI::TD_ACQUIRE ||
                            call_type == ThreadAPI::TD_TRY_ACQUIRE ||
                            call_type == ThreadAPI::TD_RWLOCK_RDLOCK ||
                            call_type == ThreadAPI::TD_RWLOCK_WRLOCK ||
                            call_type == ThreadAPI::TD_RELEASE ||
                            call_type == ThreadAPI::TD_SEMAPHORE_ACQUIRE ||
                            call_type == ThreadAPI::TD_SEMAPHORE_RELEASE ||
                            call_type == ThreadAPI::TD_SEMAPHORE_TRY_ACQUIRE ||
                            call_type == ThreadAPI::TD_KERNEL_SPIN_LOCK ||
                            call_type == ThreadAPI::TD_KERNEL_SPIN_TRYLOCK ||
                            call_type == ThreadAPI::TD_KERNEL_MUTEX_LOCK ||
                            call_type == ThreadAPI::TD_KERNEL_MUTEX_TRYLOCK ||
                            call_type == ThreadAPI::TD_KERNEL_DOWN ||
                            call_type == ThreadAPI::TD_KERNEL_READ_LOCK ||
                            call_type == ThreadAPI::TD_KERNEL_WRITE_LOCK ||
                            call_type == ThreadAPI::TD_KERNEL_DOWN_READ ||
                            call_type == ThreadAPI::TD_KERNEL_DOWN_WRITE ||
                            call_type == ThreadAPI::TD_KERNEL_SPIN_UNLOCK ||
                            call_type == ThreadAPI::TD_KERNEL_MUTEX_UNLOCK ||
                            call_type == ThreadAPI::TD_KERNEL_UP ||
                            call_type == ThreadAPI::TD_KERNEL_READ_UNLOCK ||
                            call_type == ThreadAPI::TD_KERNEL_WRITE_UNLOCK ||
                            call_type == ThreadAPI::TD_KERNEL_UP_READ ||
                            call_type == ThreadAPI::TD_KERNEL_UP_WRITE;

  if (m_thread_api->isTDAcquire(inst) && raw_lock_api) {
    if (!m_thread_api->isTryLock(inst) || !is_must) {
      if (LockID lock = getLockValue(inst)) {
        out_set.insert(lock);
      }
    }
  } else if (m_thread_api->isTDRelease(inst)) {
    if (LockID lock = getLockValue(inst)) {
      out_set.erase(lock);
      if (is_must && m_alias_analysis) {
        LockSet to_remove;
        for (const auto *l : out_set) {
          if (mayAlias(l, lock)) {
            to_remove.insert(l);
          }
        }
        for (const auto *l : to_remove) {
          out_set.erase(l);
        }
      }
    }
  } else if (m_thread_api->isTDCondWait(inst)) {
  } else if (call) {
    ThreadAPI::TD_TYPE type = call_type;

    switch (type) {
    case ThreadAPI::TD_SHARED_RDLOCK:
    case ThreadAPI::TD_SHARED_WRLOCK:
      if (LockID lock = getLockValue(inst))
        out_set.insert(lock);
      return out_set;

    case ThreadAPI::TD_SHARED_UNLOCK:
      if (LockID lock = getLockValue(inst)) {
        out_set.erase(lock);
        if (is_must && m_alias_analysis) {
          LockSet to_remove;
          for (const auto *l : out_set)
            if (mayAlias(l, lock))
              to_remove.insert(l);
          for (const auto *l : to_remove)
            out_set.erase(l);
        }
      }
      return out_set;

    case ThreadAPI::TD_LOCK_GUARD_CTOR:
    case ThreadAPI::TD_UNIQUE_LOCK_CTOR:
    case ThreadAPI::TD_SCOPED_LOCK_CTOR:
    case ThreadAPI::TD_SHARED_LOCK_CTOR: {
      auto shouldAddAtCtor = [&](LockID lock,
                                 RAIILock::OwnershipKind ownership) {
        switch (ownership) {
        case RAIILock::OwnershipKind::Immediate:
          return true;
        case RAIILock::OwnershipKind::Deferred:
          return false;
        case RAIILock::OwnershipKind::Try:
          return !is_must;
        case RAIILock::OwnershipKind::Adopt:
          for (const auto *held : in_set) {
            if (held == lock || mayAlias(held, lock)) {
              return true;
            }
          }
          return false;
        case RAIILock::OwnershipKind::Unknown:
          return !is_must;
        }
        return false;
      };

      const Function *parent_func = inst->getFunction();
      auto raii_it = m_raii_locks.find(parent_func);
      if (raii_it != m_raii_locks.end()) {
        for (const auto &raii_entry : raii_it->second) {
          const RAIILock::LockLifetime &lifetime = raii_entry.second;
          if (lifetime.constructor == call &&
              !lifetime.underlyingLocks.empty()) {
            for (const Value *underlying : lifetime.underlyingLocks) {
              if (LockID lock = getCanonicalLock(underlying)) {
                if (shouldAddAtCtor(lock, lifetime.ownership)) {
                  out_set.insert(lock);
                }
              }
            }
            return out_set;
          }
        }
      }

      RAIILock::OwnershipKind fallback_ownership =
          RAIILock::RAIILockTracker::getOwnershipKind(call);
      for (unsigned idx = 1; idx < call->arg_size(); ++idx) {
        if (LockID lock = getCanonicalLock(call->getArgOperand(idx))) {
          if (shouldAddAtCtor(lock, fallback_ownership)) {
            out_set.insert(lock);
          }
        }
      }
      return out_set;
    }

    case ThreadAPI::TD_LOCK_GUARD_DTOR:
    case ThreadAPI::TD_UNIQUE_LOCK_DTOR:
    case ThreadAPI::TD_SCOPED_LOCK_DTOR:
    case ThreadAPI::TD_SHARED_LOCK_DTOR: {
      const Function *parent_func = inst->getFunction();
      auto raii_it = m_raii_locks.find(parent_func);
      if (raii_it != m_raii_locks.end()) {
        for (const auto &raii_entry : raii_it->second) {
          const RAIILock::LockLifetime &lifetime = raii_entry.second;
          for (const Instruction *dtor : lifetime.destructors) {
            if (dtor != inst || lifetime.underlyingLocks.empty()) {
              continue;
            }
            for (const Value *underlying : lifetime.underlyingLocks) {
              LockID lock = getCanonicalLock(underlying);
              if (!lock) {
                continue;
              }
              out_set.erase(lock);
              if (is_must && m_alias_analysis) {
                LockSet to_remove;
                for (const auto *l : out_set) {
                  if (mayAlias(l, lock)) {
                    to_remove.insert(l);
                  }
                }
                for (const auto *l : to_remove) {
                  out_set.erase(l);
                }
              }
            }
            return out_set;
          }
        }
      }
      if (LockID lock = getCppWrapperLockValue(inst)) {
        out_set.erase(lock);
        if (is_must && m_alias_analysis) {
          LockSet to_remove;
          for (const auto *l : out_set) {
            if (mayAlias(l, lock)) {
              to_remove.insert(l);
            }
          }
          for (const auto *l : to_remove) {
            out_set.erase(l);
          }
        }
        return out_set;
      }
      if (is_must) {
        out_set.clear();
      }
      return out_set;
    }

    case ThreadAPI::TD_UNIQUE_LOCK_LOCK:
      if (LockID lock = getCppWrapperLockValue(inst)) {
        out_set.insert(lock);
      }
      return out_set;

    case ThreadAPI::TD_UNIQUE_LOCK_UNLOCK:
      if (LockID lock = getCppWrapperLockValue(inst)) {
        out_set.erase(lock);
        if (is_must && m_alias_analysis) {
          LockSet to_remove;
          for (const auto *l : out_set)
            if (mayAlias(l, lock))
              to_remove.insert(l);
          for (const auto *l : to_remove)
            out_set.erase(l);
        }
      }
      return out_set;

    case ThreadAPI::TD_CALL_ONCE:
    case ThreadAPI::TD_FUTURE_GET:
    case ThreadAPI::TD_FUTURE_WAIT:
    case ThreadAPI::TD_PROMISE_SET:
    case ThreadAPI::TD_LATCH_WAIT:
    case ThreadAPI::TD_LATCH_ARRIVE_WAIT:
    case ThreadAPI::TD_BARRIER_ARRIVE_WAIT:
    case ThreadAPI::TD_BARRIER_WAIT_CPP20:
    case ThreadAPI::TD_OMP_TASKWAIT:
    case ThreadAPI::TD_OMP_TASKWAIT_DEPS:
    case ThreadAPI::TD_OMP_TASKGROUP_END:
    case ThreadAPI::TD_OMP_FLUSH:
      return out_set;

    default:
      break;
    }

    if (!m_thread_api->isTDAcquire(call) && !m_thread_api->isTDRelease(call) &&
        !m_thread_api->isTDCondWait(call)) {
      auto callees = getCallees(call);
      if (callees.empty()) {
        if (is_must && shouldInvalidateMustLockState(call)) {
          out_set.clear();
        }
        return out_set;
      }
      std::vector<LockSet> callee_results;
      for (Function *callee : callees) {
        if (!callee || callee->isDeclaration())
          continue;
        LockSet candidate = out_set;
        auto it = m_function_summaries.find(callee);
        if (it != m_function_summaries.end() && it->second.is_analyzed) {
          if (!is_must) {
            LockSet may_only = candidate;
            LockSet must_dummy = candidate;
            applyFunctionSummary(call, callee, may_only, must_dummy);
            candidate = std::move(may_only);
          } else {
            LockSet may_dummy = candidate;
            LockSet must_only = candidate;
            applyFunctionSummary(call, callee, may_dummy, must_only);
            candidate = std::move(must_only);
          }
        }
        callee_results.push_back(std::move(candidate));
      }
      if (!callee_results.empty()) {
        out_set = merge(callee_results, is_must);
      }
      if (is_must && shouldInvalidateMustLockState(call)) {
        out_set.clear();
      }
    }
  }

  return out_set;
}

void LockSetAnalysis::transferReadWrite(const Instruction *inst,
                                        const LockSet &in_read,
                                        const LockSet &in_write,
                                        LockSet &out_read, LockSet &out_write,
                                        bool is_must) const {
  out_read = in_read;
  out_write = in_write;
  auto eraseReleasedLocks = [&](const std::vector<LockID> &locks) {
    for (LockID lock : locks) {
      out_read.erase(lock);
      out_write.erase(lock);
      if (is_must && m_alias_analysis) {
        LockSet to_remove_r, to_remove_w;
        for (const auto *held : out_read) {
          if (mayAlias(held, lock)) {
            to_remove_r.insert(held);
          }
        }
        for (const auto *held : out_write) {
          if (mayAlias(held, lock)) {
            to_remove_w.insert(held);
          }
        }
        for (const auto *held : to_remove_r) {
          out_read.erase(held);
        }
        for (const auto *held : to_remove_w) {
          out_write.erase(held);
        }
      }
    }
  };

  std::vector<LockID> raii_releases = getRAIILocksReleasedAt(inst);
  if (!raii_releases.empty()) {
    eraseReleasedLocks(raii_releases);
    return;
  }
  if (is_must) {
    eraseReleasedLocks(getImpreciseRAIILocksEndingAt(inst));
  }

  const CallBase *call = dyn_cast<CallBase>(inst);
  ThreadAPI::TD_TYPE call_type =
      call ? m_thread_api->getType(call) : ThreadAPI::TD_DUMMY;
  if (detail::isNonBinarySemaphoreOp(m_thread_api, inst)) {
    return;
  }
  const bool raw_lock_api = call_type == ThreadAPI::TD_ACQUIRE ||
                            call_type == ThreadAPI::TD_TRY_ACQUIRE ||
                            call_type == ThreadAPI::TD_RWLOCK_RDLOCK ||
                            call_type == ThreadAPI::TD_RWLOCK_WRLOCK ||
                            call_type == ThreadAPI::TD_RELEASE ||
                            call_type == ThreadAPI::TD_SEMAPHORE_ACQUIRE ||
                            call_type == ThreadAPI::TD_SEMAPHORE_RELEASE ||
                            call_type == ThreadAPI::TD_SEMAPHORE_TRY_ACQUIRE ||
                            call_type == ThreadAPI::TD_KERNEL_SPIN_LOCK ||
                            call_type == ThreadAPI::TD_KERNEL_SPIN_TRYLOCK ||
                            call_type == ThreadAPI::TD_KERNEL_MUTEX_LOCK ||
                            call_type == ThreadAPI::TD_KERNEL_MUTEX_TRYLOCK ||
                            call_type == ThreadAPI::TD_KERNEL_DOWN ||
                            call_type == ThreadAPI::TD_KERNEL_READ_LOCK ||
                            call_type == ThreadAPI::TD_KERNEL_WRITE_LOCK ||
                            call_type == ThreadAPI::TD_KERNEL_DOWN_READ ||
                            call_type == ThreadAPI::TD_KERNEL_DOWN_WRITE ||
                            call_type == ThreadAPI::TD_KERNEL_SPIN_UNLOCK ||
                            call_type == ThreadAPI::TD_KERNEL_MUTEX_UNLOCK ||
                            call_type == ThreadAPI::TD_KERNEL_UP ||
                            call_type == ThreadAPI::TD_KERNEL_READ_UNLOCK ||
                            call_type == ThreadAPI::TD_KERNEL_WRITE_UNLOCK ||
                            call_type == ThreadAPI::TD_KERNEL_UP_READ ||
                            call_type == ThreadAPI::TD_KERNEL_UP_WRITE;

  if (m_thread_api->isReadLockAcquire(inst)) {
    if (!m_thread_api->isTryLock(inst) || !is_must) {
      if (LockID lock = getLockValue(inst))
        out_read.insert(lock);
    }
  } else if (m_thread_api->isWriteLockAcquire(inst)) {
    if (!m_thread_api->isTryLock(inst) || !is_must) {
      if (LockID lock = getLockValue(inst)) {
        out_write.insert(lock);
        for (const auto *l : in_read) {
          if (mayAlias(l, lock))
            out_read.erase(l);
        }
      }
    }
  } else if (m_thread_api->isTDAcquire(inst) && raw_lock_api) {
    if (!m_thread_api->isTryLock(inst) || !is_must) {
      if (LockID lock = getLockValue(inst))
        out_write.insert(lock);
    }
  } else if (m_thread_api->isTDCondWait(inst)) {
  } else if (m_thread_api->isTDRelease(inst)) {
    if (LockID lock = getLockValue(inst)) {
      out_read.erase(lock);
      out_write.erase(lock);
      if (is_must && m_alias_analysis) {
        LockSet to_remove_r, to_remove_w;
        for (const auto *l : out_read)
          if (mayAlias(l, lock))
            to_remove_r.insert(l);
        for (const auto *l : out_write)
          if (mayAlias(l, lock))
            to_remove_w.insert(l);
        for (const auto *l : to_remove_r)
          out_read.erase(l);
        for (const auto *l : to_remove_w)
          out_write.erase(l);
      }
    }
  } else if (call) {
    ThreadAPI::TD_TYPE type = call_type;

    switch (type) {
    case ThreadAPI::TD_SHARED_RDLOCK:
      if (call->arg_size() >= 1) {
        if (LockID lock = getCanonicalLock(call->getArgOperand(0)))
          out_read.insert(lock);
      }
      return;

    case ThreadAPI::TD_SHARED_WRLOCK:
      if (call->arg_size() >= 1) {
        if (LockID lock = getCanonicalLock(call->getArgOperand(0))) {
          out_write.insert(lock);
          for (const auto *l : in_read) {
            if (mayAlias(l, lock))
              out_read.erase(l);
          }
        }
      }
      return;

    case ThreadAPI::TD_SHARED_UNLOCK:
      if (call->arg_size() >= 1) {
        if (LockID lock = getCanonicalLock(call->getArgOperand(0))) {
          out_read.erase(lock);
          out_write.erase(lock);
          if (is_must && m_alias_analysis) {
            LockSet to_remove_r, to_remove_w;
            for (const auto *l : out_read)
              if (mayAlias(l, lock))
                to_remove_r.insert(l);
            for (const auto *l : out_write)
              if (mayAlias(l, lock))
                to_remove_w.insert(l);
            for (const auto *l : to_remove_r)
              out_read.erase(l);
            for (const auto *l : to_remove_w)
              out_write.erase(l);
          }
        }
      }
      return;

    case ThreadAPI::TD_SHARED_LOCK_CTOR: {
      RAIILock::OwnershipKind ownership =
          RAIILock::RAIILockTracker::getOwnershipKind(call);
      bool should_add = ownership == RAIILock::OwnershipKind::Immediate ||
                        (!is_must &&
                         (ownership == RAIILock::OwnershipKind::Try ||
                          ownership == RAIILock::OwnershipKind::Unknown));
      if (!should_add && ownership != RAIILock::OwnershipKind::Adopt) {
        break;
      }
      for (unsigned idx = 1; idx < call->arg_size(); ++idx) {
        if (LockID lock = getCanonicalLock(call->getArgOperand(idx))) {
          if (ownership == RAIILock::OwnershipKind::Adopt) {
            bool held = false;
            for (const auto *candidate : in_read) {
              if (mayAlias(candidate, lock)) {
                held = true;
                break;
              }
            }
            if (!held) {
              continue;
            }
          }
          out_read.insert(lock);
        }
      }
      return;
    }

    case ThreadAPI::TD_LOCK_GUARD_CTOR:
    case ThreadAPI::TD_UNIQUE_LOCK_CTOR:
    case ThreadAPI::TD_SCOPED_LOCK_CTOR: {
      RAIILock::OwnershipKind ownership =
          RAIILock::RAIILockTracker::getOwnershipKind(call);
      bool should_add = ownership == RAIILock::OwnershipKind::Immediate ||
                        (!is_must &&
                         (ownership == RAIILock::OwnershipKind::Try ||
                          ownership == RAIILock::OwnershipKind::Unknown));
      if (ownership == RAIILock::OwnershipKind::Deferred) {
        should_add = false;
      }
      if (!should_add && ownership != RAIILock::OwnershipKind::Adopt) {
        break;
      }
      for (unsigned idx = 1; idx < call->arg_size(); ++idx) {
        if (LockID lock = getCanonicalLock(call->getArgOperand(idx))) {
          if (ownership == RAIILock::OwnershipKind::Adopt) {
            bool held = false;
            for (const auto *candidate : in_write) {
              if (mayAlias(candidate, lock)) {
                held = true;
                break;
              }
            }
            if (!held) {
              continue;
            }
          }
          out_write.insert(lock);
        }
      }
      return;
    }

    case ThreadAPI::TD_SHARED_LOCK_DTOR:
    case ThreadAPI::TD_LOCK_GUARD_DTOR:
    case ThreadAPI::TD_UNIQUE_LOCK_DTOR:
    case ThreadAPI::TD_SCOPED_LOCK_DTOR:
    case ThreadAPI::TD_UNIQUE_LOCK_UNLOCK: {
      std::vector<LockID> locks =
          getUnderlyingRAIILocks(inst, call->getArgOperand(0));
      if (locks.empty()) {
        if (LockID lock = getCppWrapperLockValue(inst)) {
          locks.push_back(lock);
        }
      }
      if (locks.empty() && is_must) {
        out_read.clear();
        out_write.clear();
        break;
      }
      for (LockID lock : locks) {
        out_read.erase(lock);
        out_write.erase(lock);
        if (is_must && m_alias_analysis) {
          LockSet to_remove_r, to_remove_w;
          for (const auto *l : out_read)
            if (mayAlias(l, lock))
              to_remove_r.insert(l);
          for (const auto *l : out_write)
            if (mayAlias(l, lock))
              to_remove_w.insert(l);
          for (const auto *l : to_remove_r)
            out_read.erase(l);
          for (const auto *l : to_remove_w)
            out_write.erase(l);
        }
      }
      return;
    }

    case ThreadAPI::TD_UNIQUE_LOCK_LOCK: {
      std::vector<LockID> locks =
          getUnderlyingRAIILocks(inst, call->getArgOperand(0));
      if (locks.empty()) {
        if (LockID lock = getCppWrapperLockValue(inst)) {
          locks.push_back(lock);
        }
      }
      for (LockID lock : locks) {
        out_write.insert(lock);
      }
      return;
    }

    default:
      break;
    }

    auto applySummaryToReadWrite = [&](const Function *callee,
                                       LockSet &candidate_read,
                                       LockSet &candidate_write) -> bool {
      auto it = m_function_summaries.find(callee);
      if (it == m_function_summaries.end() || !it->second.is_analyzed) {
        return false;
      }

      auto eraseMustReleasedLock = [&](LockID released_lock) {
        if (!released_lock) {
          return;
        }
        candidate_read.erase(released_lock);
        candidate_write.erase(released_lock);
        if (!m_alias_analysis) {
          return;
        }

        LockSet aliased_read;
        LockSet aliased_write;
        for (const auto *held : candidate_read) {
          if (mayAlias(held, released_lock)) {
            aliased_read.insert(held);
          }
        }
        for (const auto *held : candidate_write) {
          if (mayAlias(held, released_lock)) {
            aliased_write.insert(held);
          }
        }
        for (const auto *held : aliased_read) {
          candidate_read.erase(held);
        }
        for (const auto *held : aliased_write) {
          candidate_write.erase(held);
        }
      };

      const FunctionSummary &summary = it->second;
      if (!is_must) {
        candidate_read.insert(summary.may_read_acquire_delta.begin(),
                              summary.may_read_acquire_delta.end());
        candidate_write.insert(summary.may_write_acquire_delta.begin(),
                               summary.may_write_acquire_delta.end());
      } else {
        candidate_read.insert(summary.must_read_acquire_delta.begin(),
                              summary.must_read_acquire_delta.end());
        candidate_write.insert(summary.must_write_acquire_delta.begin(),
                               summary.must_write_acquire_delta.end());
        for (LockID lock : summary.may_release_delta) {
          eraseMustReleasedLock(lock);
        }
      }

      for (LockID lock : summary.must_release_delta) {
        eraseMustReleasedLock(lock);
      }
      return true;
    };

    auto callees = getCallees(call);
    std::vector<LockSet> read_results;
    std::vector<LockSet> write_results;
    for (Function *callee : callees) {
      if (!callee || callee->isDeclaration()) {
        continue;
      }
      LockSet candidate_read = out_read;
      LockSet candidate_write = out_write;
      (void)applySummaryToReadWrite(callee, candidate_read, candidate_write);
      read_results.push_back(std::move(candidate_read));
      write_results.push_back(std::move(candidate_write));
    }

    if (!read_results.empty()) {
      out_read = merge(read_results, is_must);
      out_write = merge(write_results, is_must);
    }

    if (is_must && shouldInvalidateMustLockState(call)) {
      out_read.clear();
      out_write.clear();
    }
  }
}

LockSet LockSetAnalysis::merge(const std::vector<LockSet> &sets,
                               bool is_must) const {
  if (sets.empty()) {
    return LockSet();
  }

  if (is_must) {
    auto matchesLock = [this](LockID lhs, LockID rhs) {
      const LockID clhs = getCanonicalLock(lhs);
      const LockID crhs = getCanonicalLock(rhs);
      if (clhs && crhs && clhs == crhs) {
        return true;
      }
      return m_alias_analysis && clhs && crhs &&
             m_alias_analysis->mustAlias(clhs, crhs);
    };

    LockSet result = sets[0];
    for (size_t i = 1; i < sets.size(); ++i) {
      LockSet intersection;
      for (LockID lhs : result) {
        for (LockID rhs : sets[i]) {
          if (matchesLock(lhs, rhs)) {
            intersection.insert(getCanonicalLock(lhs));
            break;
          }
        }
      }
      result = intersection;
    }
    return result;
  }

  LockSet result;
  for (const auto &set : sets) {
    result.insert(set.begin(), set.end());
  }
  return result;
}
