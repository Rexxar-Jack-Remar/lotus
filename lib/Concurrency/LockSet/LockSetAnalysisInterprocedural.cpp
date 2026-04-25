#include "Concurrency/LockSet/LockSetAnalysis.h"

#include "Alias/Infrastructure/AliasAnalysisWrapper/AliasAnalysisWrapper.h"
#include "Concurrency/LockSet/LockSetAnalysisInternal.h"

#include <queue>
#include <set>
#include <stack>

#include <llvm/Analysis/PostDominators.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/InstIterator.h>

using namespace llvm;
using namespace mhp;

void LockSetAnalysis::computeInterproceduralLockSets() {
  if (!m_call_graph) {
    errs() << "Warning: CallGraph not available. Skipping interprocedural "
              "analysis.\n";
    return;
  }

  errs() << "Computing interprocedural lock sets using CallGraph...\n";

  bottomUpTraversal();

  for (Function &func : *m_module) {
    if (!func.isDeclaration()) {
      analyzeFunction(&func);
    }
  }

  for (auto &entry : m_function_summaries) {
    entry.second.is_analyzed = false;
  }
  bottomUpTraversal();

  errs() << "Interprocedural lock set analysis complete.\n";
}

std::set<Function *> LockSetAnalysis::getCallees(const CallBase *call) const {
  std::set<Function *> callees;

  if (!call) {
    return callees;
  }

  if (Function *direct_callee = call->getCalledFunction()) {
    if (!direct_callee->isDeclaration()) {
      callees.insert(direct_callee);
    }
    return callees;
  }

  if (const Value *called = call->getCalledOperand()) {
    if (const Function *direct_target =
            dyn_cast<Function>(called->stripPointerCasts())) {
      if (!direct_target->isDeclaration()) {
        callees.insert(const_cast<Function *>(direct_target));
      }
      return callees;
    }

    std::unordered_set<const Value *> visited_values;
    detail::collectDefinedFunctionTargets(called, callees, visited_values);
  }

  if (m_call_graph) {
    Function *caller = const_cast<Function *>(call->getFunction());
    if (CallGraphNode *cgNode = (*m_call_graph)[caller]) {
      for (auto &callRecord : *cgNode) {
        if (!callRecord.first.hasValue() ||
            dyn_cast_or_null<CallBase>(*callRecord.first) != call) {
          continue;
        }
        if (Function *callee = callRecord.second->getFunction()) {
          if (!callee->isDeclaration()) {
            callees.insert(callee);
          }
        }
      }
    }
  }

  return callees;
}

bool LockSetAnalysis::shouldInvalidateMustLockState(
    const CallBase *call) const {
  if (!call) {
    return false;
  }

  const Function *direct = call->getCalledFunction();
  if (direct && direct->isIntrinsic()) {
    return false;
  }

  if (call->doesNotAccessMemory() || call->onlyReadsMemory()) {
    return false;
  }

  return getCallees(call).empty() || hasUnresolvedCalleeTarget(call);
}

bool LockSetAnalysis::hasUnresolvedCalleeTarget(const CallBase *call) const {
  if (!call) {
    return false;
  }

  if (Function *direct = call->getCalledFunction()) {
    return direct->isDeclaration() && !direct->isIntrinsic();
  }

  if (const Value *called = call->getCalledOperand()) {
    if (const auto *direct_target =
            dyn_cast<Function>(called->stripPointerCasts())) {
      return direct_target->isDeclaration() && !direct_target->isIntrinsic();
    }
  }

  if (!m_call_graph) {
    return true;
  }

  Function *caller = const_cast<Function *>(call->getFunction());
  if (CallGraphNode *cgNode = (*m_call_graph)[caller]) {
    bool matched_record = false;
    for (auto &callRecord : *cgNode) {
      if (!callRecord.first.hasValue() ||
          dyn_cast_or_null<CallBase>(*callRecord.first) != call) {
        continue;
      }
      matched_record = true;
      CallGraphNode *callee_node = callRecord.second;
      if (!callee_node) {
        return true;
      }
      Function *callee = callee_node->getFunction();
      if (!callee || (callee->isDeclaration() && !callee->isIntrinsic())) {
        return true;
      }
    }
    return !matched_record;
  }

  return true;
}

void LockSetAnalysis::computeFunctionSummary(Function *func) {
  if (!func || func->isDeclaration()) {
    return;
  }

  auto &summary = m_function_summaries[func];
  if (summary.is_analyzed) {
    return;
  }

  errs() << "Computing summary for function: " << func->getName() << "\n";

  computeIntraproceduralLockSets(func);

  summary.may_acquire_delta.clear();
  summary.may_read_acquire_delta.clear();
  summary.may_write_acquire_delta.clear();
  summary.must_acquire_delta.clear();
  summary.must_read_acquire_delta.clear();
  summary.must_write_acquire_delta.clear();
  summary.may_release_delta.clear();
  summary.must_release_delta.clear();

  auto matchesLock = [this](LockID lhs, LockID rhs) {
    const LockID clhs = getCanonicalLock(lhs);
    const LockID crhs = getCanonicalLock(rhs);
    if (clhs && crhs && clhs == crhs) {
      return true;
    }
    return m_alias_analysis && clhs && crhs &&
           m_alias_analysis->mustAlias(clhs, crhs);
  };

  auto isDefinitelyHeldAt = [&](const Instruction *inst, LockID lock) {
    auto it = m_must_locksets_entry.find(inst);
    return it != m_must_locksets_entry.end() &&
           llvm::any_of(it->second,
                        [&](LockID held) { return matchesLock(held, lock); });
  };

  auto isPossiblyHeldAt = [&](const Instruction *inst, LockID lock) {
    auto it = m_may_locksets_entry.find(inst);
    return it != m_may_locksets_entry.end() &&
           llvm::any_of(it->second,
                        [&](LockID held) { return matchesLock(held, lock); });
  };

  auto isBinarySemaphoreOnlyLockInFunction = [&](LockID lock) {
    bool saw_binary_semaphore = false;
    for (const Instruction &inst : instructions(*func)) {
      if (!m_thread_api->isTDAcquire(&inst)) {
        continue;
      }
      LockID inst_lock = getLockValue(&inst);
      if (!inst_lock || !matchesLock(inst_lock, lock)) {
        continue;
      }
      if (!m_thread_api->isBinarySemaphoreOp(&inst)) {
        return false;
      }
      saw_binary_semaphore = true;
    }
    return saw_binary_semaphore;
  };

  DominatorTree dom(*func);
  auto intersectMustSets = [&](const LockSet &lhs, const LockSet &rhs) {
    return merge({lhs, rhs}, true);
  };

  std::vector<const ReturnInst *> returns;
  for (const BasicBlock &bb : *func) {
    if (const ReturnInst *ret = dyn_cast<ReturnInst>(bb.getTerminator())) {
      returns.push_back(ret);
    }
  }

  auto returnMayObserveUnmatchedRelease =
      [&](const ReturnInst *ret,
          const std::vector<const Instruction *> &release_sites) {
        if (!ret) {
          return false;
        }
        auto blockCanReach = [](const BasicBlock *from, const BasicBlock *to) {
          if (!from || !to) {
            return false;
          }
          if (from == to) {
            return true;
          }
          std::queue<const BasicBlock *> worklist;
          std::set<const BasicBlock *> visited;
          worklist.push(from);
          visited.insert(from);
          while (!worklist.empty()) {
            const BasicBlock *current = worklist.front();
            worklist.pop();
            for (const BasicBlock *succ : successors(current)) {
              if (!visited.insert(succ).second) {
                continue;
              }
              if (succ == to) {
                return true;
              }
              worklist.push(succ);
            }
          }
          return false;
        };
        for (const Instruction *release_inst : release_sites) {
          if (!release_inst || release_inst->getFunction() != func) {
            continue;
          }
          if (release_inst->getParent() == ret->getParent()) {
            if (release_inst->comesBefore(ret)) {
              return true;
            }
            continue;
          }
          if (blockCanReach(release_inst->getParent(), ret->getParent())) {
            return true;
          }
        }
        return false;
      };

  auto returnMustObserveUnmatchedRelease =
      [&](const ReturnInst *ret,
          const std::vector<const Instruction *> &release_sites) {
        if (!ret) {
          return false;
        }
        for (const Instruction *release_inst : release_sites) {
          if (!release_inst || release_inst->getFunction() != func) {
            continue;
          }
          if (release_inst->getParent() == ret->getParent()) {
            if (release_inst->comesBefore(ret)) {
              return true;
            }
            continue;
          }
          if (dom.dominates(release_inst->getParent(), ret->getParent())) {
            return true;
          }
        }
        return false;
      };

  if (!returns.empty()) {
    bool seeded_must_read = false;
    bool seeded_must_write = false;
    LockSet must_read_intersection;
    LockSet must_write_intersection;
    for (const auto *ret : returns) {
      auto it_read = m_may_read_locks_exit.find(ret);
      if (it_read != m_may_read_locks_exit.end()) {
        for (LockID lock : it_read->second) {
          if (!isBinarySemaphoreOnlyLockInFunction(lock)) {
            summary.may_read_acquire_delta.insert(lock);
          }
        }
      }

      auto it_write = m_may_write_locks_exit.find(ret);
      if (it_write != m_may_write_locks_exit.end()) {
        for (LockID lock : it_write->second) {
          if (!isBinarySemaphoreOnlyLockInFunction(lock)) {
            summary.may_write_acquire_delta.insert(lock);
          }
        }
      }

      auto it_must_read = m_must_read_locks_exit.find(ret);
      if (it_must_read != m_must_read_locks_exit.end()) {
        if (!seeded_must_read) {
          must_read_intersection = it_must_read->second;
          seeded_must_read = true;
        } else {
          must_read_intersection =
              intersectMustSets(must_read_intersection, it_must_read->second);
        }
      } else {
        must_read_intersection.clear();
        seeded_must_read = true;
      }

      auto it_must_write = m_must_write_locks_exit.find(ret);
      if (it_must_write != m_must_write_locks_exit.end()) {
        if (!seeded_must_write) {
          must_write_intersection = it_must_write->second;
          seeded_must_write = true;
        } else {
          must_write_intersection =
              intersectMustSets(must_write_intersection, it_must_write->second);
        }
      } else {
        must_write_intersection.clear();
        seeded_must_write = true;
      }
    }

    summary.may_acquire_delta = summary.may_read_acquire_delta;
    summary.may_acquire_delta.insert(summary.may_write_acquire_delta.begin(),
                                     summary.may_write_acquire_delta.end());

    if (seeded_must_read) {
      for (LockID lock : must_read_intersection) {
        if (!isBinarySemaphoreOnlyLockInFunction(lock)) {
          summary.must_read_acquire_delta.insert(lock);
        }
      }
    }
    if (seeded_must_write) {
      for (LockID lock : must_write_intersection) {
        if (!isBinarySemaphoreOnlyLockInFunction(lock)) {
          summary.must_write_acquire_delta.insert(lock);
        }
      }
    }

    summary.must_acquire_delta = summary.must_read_acquire_delta;
    summary.must_acquire_delta.insert(summary.must_write_acquire_delta.begin(),
                                      summary.must_write_acquire_delta.end());
  }

  std::unordered_map<LockID, std::vector<const Instruction *>>
      maybe_unmatched_releases;
  std::unordered_map<LockID, std::vector<const Instruction *>>
      must_unmatched_releases;
  for (inst_iterator I = inst_begin(func), E = inst_end(func); I != E; ++I) {
    Instruction *inst = &*I;
    std::vector<LockID> released = getRAIILocksReleasedAt(inst);
    if (!released.empty()) {
      for (LockID lock : released) {
        if (!isDefinitelyHeldAt(inst, lock)) {
          maybe_unmatched_releases[getCanonicalLock(lock)].push_back(inst);
        }
        if (!isPossiblyHeldAt(inst, lock)) {
          must_unmatched_releases[getCanonicalLock(lock)].push_back(inst);
        }
      }
      continue;
    }
    if (detail::isNonBinarySemaphoreOp(m_thread_api, inst)) {
      continue;
    }
    if (m_thread_api->isTDRelease(inst) && !m_thread_api->isTDAcquire(inst)) {
      if (LockID lock = getLockValue(inst)) {
        if (!isDefinitelyHeldAt(inst, lock)) {
          maybe_unmatched_releases[getCanonicalLock(lock)].push_back(inst);
        }
        if (!isPossiblyHeldAt(inst, lock)) {
          must_unmatched_releases[getCanonicalLock(lock)].push_back(inst);
        }
      }
    }
  }

  for (const auto &entry : maybe_unmatched_releases) {
    bool reaches_any_return = false;
    for (const ReturnInst *ret : returns) {
      reaches_any_return |= returnMayObserveUnmatchedRelease(ret, entry.second);
    }
    if (reaches_any_return) {
      summary.may_release_delta.insert(entry.first);
    }
  }

  for (const auto &entry : must_unmatched_releases) {
    bool covers_all_returns = !returns.empty();
    for (const ReturnInst *ret : returns) {
      covers_all_returns &=
          returnMustObserveUnmatchedRelease(ret, entry.second);
    }
    if (covers_all_returns) {
      summary.must_release_delta.insert(entry.first);
    }
  }

  for (LockID lock : getImpreciseRAIILocksInFunction(func)) {
    summary.may_acquire_delta.erase(lock);
    summary.may_read_acquire_delta.erase(lock);
    summary.may_write_acquire_delta.erase(lock);
    summary.must_acquire_delta.erase(lock);
    summary.must_read_acquire_delta.erase(lock);
    summary.must_write_acquire_delta.erase(lock);
  }

  summary.is_analyzed = true;

  errs() << "  May acquire delta: " << summary.may_acquire_delta.size()
         << " locks\n";
  errs() << "  Must acquire delta: " << summary.must_acquire_delta.size()
         << " locks\n";
  errs() << "  May release delta: " << summary.may_release_delta.size()
         << " locks\n";
  errs() << "  Must release delta: " << summary.must_release_delta.size()
         << " locks\n";
}

void LockSetAnalysis::applyFunctionSummary(const CallBase *call,
                                           const Function *callee,
                                           LockSet &may_locks,
                                           LockSet &must_locks) const {
  if (!call || !callee) {
    return;
  }

  auto it = m_function_summaries.find(callee);
  if (it == m_function_summaries.end() || !it->second.is_analyzed) {
    return;
  }

  const FunctionSummary &summary = it->second;

  may_locks.insert(summary.may_acquire_delta.begin(),
                   summary.may_acquire_delta.end());
  must_locks.insert(summary.must_acquire_delta.begin(),
                    summary.must_acquire_delta.end());

  for (LockID lock : summary.may_release_delta) {
    must_locks.erase(lock);
    if (m_alias_analysis) {
      LockSet to_remove;
      for (const auto *l : must_locks) {
        if (mayAlias(l, lock)) {
          to_remove.insert(l);
        }
      }
      for (const auto *l : to_remove) {
        must_locks.erase(l);
      }
    }
  }

  for (LockID lock : summary.must_release_delta) {
    may_locks.erase(lock);
    must_locks.erase(lock);
    if (m_alias_analysis) {
      LockSet to_remove;
      for (const auto *l : must_locks) {
        if (mayAlias(l, lock)) {
          to_remove.insert(l);
        }
      }
      for (const auto *l : to_remove) {
        must_locks.erase(l);
      }
    }
  }
}

void LockSetAnalysis::bottomUpTraversal() {
  if (!m_call_graph) {
    return;
  }

  errs() << "Performing bottom-up call graph traversal...\n";

  std::vector<Function *> post_order;
  std::set<Function *> visited;
  std::stack<std::pair<Function *, bool>> stack;

  for (Function &func : *m_module) {
    if (func.isDeclaration() || visited.find(&func) != visited.end()) {
      continue;
    }
    stack.push({&func, false});

    while (!stack.empty()) {
      auto [current, children_visited] = stack.top();
      stack.pop();

      if (children_visited) {
        post_order.push_back(current);
        continue;
      }

      if (visited.find(current) != visited.end()) {
        continue;
      }

      visited.insert(current);
      stack.push({current, true});

      if (CallGraphNode *cgNode = (*m_call_graph)[current]) {
        for (auto &callRecord : *cgNode) {
          if (Function *callee = callRecord.second->getFunction()) {
            if (callee && !callee->isDeclaration() &&
                visited.find(callee) == visited.end()) {
              stack.push({callee, false});
            }
          }
        }
      }
    }
  }

  errs() << "Processing " << post_order.size()
         << " functions in bottom-up order\n";

  for (Function *func : post_order) {
    computeFunctionSummary(func);
  }

  errs() << "Bottom-up traversal complete\n";
}
