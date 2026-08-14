/**
 * @file LinuxKernelWaitAnalysis.cpp
 * @brief Linux Kernel Wait Analysis Implementation
 *
 * @author Lotus Analysis Framework
 * @date 2026
 */

#include "Concurrency/LinuxKernel/LinuxKernelWaitAnalysis.h"

#include "Concurrency/LinuxKernel/LinuxKernelProcessModel.h"

#include <algorithm>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <llvm/IR/Dominators.h>

using namespace llvm;

namespace kernel {

void LinuxKernelWaitAnalysis::analyzeWaits() {
  wait_contexts_.clear();
  completion_map_.clear();
  timer_map_.clear();
  wait_diagnostics_.clear();

  identifyWaitContexts();
  identifyCompletions();
  identifyTimers();

  wait_diagnostics_["total_waits"] = wait_contexts_.size();
  wait_diagnostics_["total_completions"] = completion_map_.size();
  wait_diagnostics_["total_timers"] = timer_map_.size();
}

void LinuxKernelWaitAnalysis::identifyWaitContexts() {
  for (const auto &op : process_model_.getAllOperations()) {
    if (op.kind != OperationKind::WAIT_EVENT &&
        op.kind != OperationKind::PREPARE_WAIT) {
      continue;
    }

    WaitContext ctx;
    ctx.wait_inst = op.inst;
    ctx.wait_queue = op.wait_queue;
    ctx.condition = op.wait_condition;
    ctx.interruptible = op.is_interruptible;
    ctx.has_timeout = op.has_timeout;
    wait_contexts_.push_back(ctx);
  }

  for (const auto &op : process_model_.getAllOperations()) {
    if (op.kind != OperationKind::WAKE_UP || op.wait_queue == nullptr) {
      continue;
    }

    for (auto &ctx : wait_contexts_) {
      if (ctx.wait_queue == op.wait_queue) {
        ctx.wake_insts.push_back(op.inst);
      }
    }
  }
}

void LinuxKernelWaitAnalysis::identifyCompletions() {
  for (const auto &op : process_model_.getAllOperations()) {
    if (op.wait_queue == nullptr) {
      continue;
    }

    if (op.kind == OperationKind::COMPLETION_INIT) {
      auto &ctx = completion_map_[op.wait_queue];
      ctx.init_inst = op.inst;
      ctx.is_done = false;
      ctx.completers.clear();
    } else if (op.kind == OperationKind::COMPLETION_REINIT) {
      auto &ctx = completion_map_[op.wait_queue];
      ctx.is_done = false;
      ctx.completers.clear();
    } else if (op.kind == OperationKind::COMPLETION_WAIT) {
      completion_map_[op.wait_queue].waiters.push_back(op.inst);
    } else if (op.kind == OperationKind::COMPLETION_SIGNAL) {
      auto &ctx = completion_map_[op.wait_queue];
      ctx.completers.push_back(op.inst);
      ctx.is_done = true;
    }
  }
}

void LinuxKernelWaitAnalysis::identifyTimers() {
  for (const auto &op : process_model_.getAllOperations()) {
    if (op.wait_queue == nullptr) {
      continue;
    }

    auto &ctx = timer_map_[op.wait_queue];
    if (op.kind == OperationKind::TIMER_SETUP) {
      ctx.setup_inst = op.inst;
    } else if (op.kind == OperationKind::TIMER_MOD) {
      ctx.mod_inst = op.inst;
      ctx.expires = op.timer_expires;
    } else if (op.kind == OperationKind::TIMER_DELETE) {
      ctx.delete_inst = op.inst;
    } else if (op.kind == OperationKind::TIMER_SHUTDOWN) {
      ctx.shutdown_inst = op.inst;
    }
  }
}

std::vector<std::pair<const Instruction *, const Instruction *>>
LinuxKernelWaitAnalysis::findMissingWakeUps() const {
  // A missing call in this module is not a lost wakeup.  Diagnosing one needs
  // the condition's memory accesses, enqueue/check ordering and MHP/HB data.
  return {};
}

std::vector<std::pair<const Instruction *, const Instruction *>>
LinuxKernelWaitAnalysis::findSpuriousWakeUps() const {
  // A wake with no statically visible waiter is legal: waiters may be in
  // another translation unit, may arrive later, or the wake may race with
  // condition evaluation.  Reporting it requires whole-program MHP/HB data.
  return {};
}

std::vector<const Instruction *>
LinuxKernelWaitAnalysis::findMissingCompletion() const {
  // Absence of a completer in the current module is not proof that a wait can
  // never complete.  Keep this query conservative until callback/MHP modeling
  // can establish a closed set of producers.
  return {};
}

std::vector<const Instruction *>
LinuxKernelWaitAnalysis::findDoubleCompletion() const {
  std::vector<const Instruction *> result;
  std::map<const Function *, std::unique_ptr<DominatorTree>> dominators;

  for (const auto &second : process_model_.getAllOperations()) {
    if (second.kind != OperationKind::COMPLETION_SIGNAL ||
        second.completion_signal != CompletionSignalKind::ALL ||
        second.wait_queue == nullptr) {
      continue;
    }
    const Function *function = second.inst->getFunction();
    auto &dt = dominators[function];
    if (!dt) {
      dt = std::make_unique<DominatorTree>(*const_cast<Function *>(function));
    }

    for (const auto &first : process_model_.getAllOperations()) {
      if (&first == &second || first.kind != OperationKind::COMPLETION_SIGNAL ||
          first.completion_signal != CompletionSignalKind::ALL ||
          first.wait_queue != second.wait_queue ||
          first.inst->getFunction() != function ||
          !dt->dominates(first.inst, second.inst)) {
        continue;
      }

      bool reinitialized = false;
      for (const auto &reset : process_model_.getAllOperations()) {
        if ((reset.kind == OperationKind::COMPLETION_INIT ||
             reset.kind == OperationKind::COMPLETION_REINIT) &&
            reset.wait_queue == second.wait_queue &&
            reset.inst->getFunction() == function &&
            dt->dominates(first.inst, reset.inst) &&
            dt->dominates(reset.inst, second.inst)) {
          reinitialized = true;
          break;
        }
      }
      if (!reinitialized) {
        result.push_back(second.inst);
        break;
      }
    }
  }
  return result;
}

std::vector<const Instruction *>
LinuxKernelWaitAnalysis::findTimerNotDeleted() const {
  // A pending timer is not intrinsically a leak; its owner may remain live for
  // the rest of the module or system lifetime.
  return {};
}

std::vector<const Instruction *>
LinuxKernelWaitAnalysis::findTimerUseAfterDelete() const {
  std::vector<const Instruction *> result;
  std::map<const Function *, std::unique_ptr<DominatorTree>> dominators;
  for (const auto &mod : process_model_.getAllOperations()) {
    if (mod.kind != OperationKind::TIMER_MOD || mod.wait_queue == nullptr) {
      continue;
    }
    const Function *function = mod.inst->getFunction();
    auto &dt = dominators[function];
    if (!dt) {
      dt = std::make_unique<DominatorTree>(*const_cast<Function *>(function));
    }
    for (const auto &shutdown : process_model_.getAllOperations()) {
      if (shutdown.kind == OperationKind::TIMER_SHUTDOWN &&
          shutdown.wait_queue == mod.wait_queue &&
          shutdown.inst->getFunction() == function &&
          dt->dominates(shutdown.inst, mod.inst)) {
        result.push_back(mod.inst);
        break;
      }
    }
  }
  return result;
}

std::vector<std::pair<const Instruction *, const Instruction *>>
LinuxKernelWaitAnalysis::findRaceBetweenWaitAndWake() const {
  std::vector<std::pair<const Instruction *, const Instruction *>> result;
  // Program order between calls is insufficient to prove a lost wakeup.  The
  // condition access, enqueue state and inter-thread happens-before relation
  // must all participate in this diagnostic.
  return result;
}

} // namespace kernel
