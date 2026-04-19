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
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

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
  std::map<WaitQueueID, size_t> last_wait_context_by_queue;

  for (const auto &op : process_model_.getAllOperations()) {
    if (op.kind != OperationKind::WAIT_EVENT &&
        op.kind != OperationKind::PREPARE_WAIT) {
      continue;
    }

    WaitContext ctx;
    ctx.wait_inst = op.inst;
    ctx.wait_queue = op.wait_queue;
    ctx.interruptible = op.is_interruptible;
    ctx.has_timeout = op.has_timeout;
    ctx.wake_inst = nullptr;

    const size_t context_index = wait_contexts_.size();
    wait_contexts_.push_back(ctx);
    last_wait_context_by_queue[op.wait_queue] = context_index;
  }

  for (const auto &op : process_model_.getAllOperations()) {
    if (op.kind != OperationKind::WAKE_UP || op.wait_queue == nullptr) {
      continue;
    }

    auto it = last_wait_context_by_queue.find(op.wait_queue);
    if (it != last_wait_context_by_queue.end()) {
      auto &ctx = wait_contexts_[it->second];
      if (ctx.wake_inst == nullptr ||
          process_model_.isBeforeInFunction(ctx.wake_inst, op.inst)) {
        ctx.wake_inst = op.inst;
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
      ctx.delay_ms = op.timer_delay_ms;
    } else if (op.kind == OperationKind::TIMER_DELETE) {
      ctx.delete_inst = op.inst;
    }
  }
}

std::vector<std::pair<const Instruction *, const Instruction *>>
LinuxKernelWaitAnalysis::findMissingWakeUps() const {
  std::vector<std::pair<const Instruction *, const Instruction *>> result;
  for (const auto &ctx : wait_contexts_) {
    if (!ctx.has_timeout && ctx.wake_inst == nullptr) {
      result.emplace_back(ctx.wait_inst, nullptr);
    }
  }
  return result;
}

std::vector<std::pair<const Instruction *, const Instruction *>>
LinuxKernelWaitAnalysis::findSpuriousWakeUps() const {
  std::vector<std::pair<const Instruction *, const Instruction *>> result;
  for (const auto &op : process_model_.getAllOperations()) {
    if (op.kind != OperationKind::WAKE_UP) {
      continue;
    }

    bool matched_wait = false;
    for (const auto &ctx : wait_contexts_) {
      if (ctx.wait_queue == op.wait_queue) {
        matched_wait = true;
        break;
      }
    }

    if (!matched_wait) {
      result.emplace_back(nullptr, op.inst);
    }
  }
  return result;
}

std::vector<const Instruction *>
LinuxKernelWaitAnalysis::findMissingCompletion() const {
  std::vector<const Instruction *> result;

  for (const auto &pair : completion_map_) {
    const CompletionContext &ctx = pair.second;
    if (ctx.waiters.size() > 0 && ctx.completers.empty()) {
      for (const auto *wait : ctx.waiters) {
        result.push_back(wait);
      }
    }
  }

  return result;
}

std::vector<const Instruction *>
LinuxKernelWaitAnalysis::findDoubleCompletion() const {
  std::vector<const Instruction *> result;
  for (const auto &pair : completion_map_) {
    const CompletionContext &ctx = pair.second;
    if (ctx.completers.size() <= 1) {
      continue;
    }
    result.insert(result.end(), ctx.completers.begin() + 1, ctx.completers.end());
  }
  return result;
}

std::vector<const Instruction *>
LinuxKernelWaitAnalysis::findTimerNotDeleted() const {
  std::vector<const Instruction *> result;

  for (const auto &pair : timer_map_) {
    const TimerContext &ctx = pair.second;
    if (ctx.delete_inst == nullptr && ctx.mod_inst != nullptr) {
      result.push_back(ctx.mod_inst);
    }
  }

  return result;
}

std::vector<const Instruction *>
LinuxKernelWaitAnalysis::findTimerUseAfterDelete() const {
  std::vector<const Instruction *> result;
  for (const auto &pair : timer_map_) {
    const TimerContext &ctx = pair.second;
    if (ctx.delete_inst == nullptr) {
      continue;
    }
    if (ctx.mod_inst != nullptr &&
        process_model_.isBeforeInFunction(ctx.delete_inst, ctx.mod_inst)) {
      result.push_back(ctx.mod_inst);
    }
  }
  return result;
}

std::vector<std::pair<const Instruction *, const Instruction *>>
LinuxKernelWaitAnalysis::findRaceBetweenWaitAndWake() const {
  std::vector<std::pair<const Instruction *, const Instruction *>> result;
  for (const auto &ctx : wait_contexts_) {
    if (ctx.wake_inst == nullptr || ctx.wait_queue == nullptr) {
      continue;
    }
    if (ctx.wait_inst->getFunction() == ctx.wake_inst->getFunction() &&
        process_model_.isBeforeInFunction(ctx.wake_inst, ctx.wait_inst)) {
      result.emplace_back(ctx.wait_inst, ctx.wake_inst);
    }
  }
  return result;
}

} // namespace kernel
