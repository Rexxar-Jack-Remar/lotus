/**
 * @file LinuxKernelExecutionGraph.cpp
 * @brief Linux kernel callback execution-context graph implementation.
 */

#include "Concurrency/LinuxKernel/LinuxKernelExecutionGraph.h"

#include "Concurrency/LinuxKernel/LinuxKernelProcessModel.h"
#include "Concurrency/MHP/HappensBeforeAnalysis.h"
#include "Concurrency/MHP/IMHPAnalysis.h"

#include <deque>

#include <llvm/Analysis/CFG.h>
#include <llvm/Analysis/PostDominators.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Instructions.h>

using namespace llvm;

namespace kernel {

namespace {

const Function *resolveCallback(const Value *value) {
  if (value == nullptr) {
    return nullptr;
  }
  return dyn_cast<Function>(value->stripPointerCasts());
}

LinuxKernelExecutionGraph::EdgeKind edgeKindFor(const KernelOperation &op) {
  switch (op.kind) {
  case OperationKind::KTHREAD_START:
  case OperationKind::KTHREAD_RUN:
    return LinuxKernelExecutionGraph::EdgeKind::SPAWN;
  case OperationKind::WORKqueue_SUBMIT:
  case OperationKind::TIMER_MOD:
  case OperationKind::RCU_CALL:
  case OperationKind::SOFTIRQ_RAISE:
  case OperationKind::TASKLET_SCHEDULE:
  case OperationKind::NAPI_SCHEDULE:
    return LinuxKernelExecutionGraph::EdgeKind::SUBMIT;
  case OperationKind::KTHREAD_STOP:
    return LinuxKernelExecutionGraph::EdgeKind::JOIN;
  case OperationKind::WORKqueue_FLUSH:
  case OperationKind::RCU_BARRIER:
    return LinuxKernelExecutionGraph::EdgeKind::FLUSH;
  case OperationKind::WORKqueue_CANCEL:
  case OperationKind::WORKqueue_DESTROY:
  case OperationKind::IRQ_FREE:
  case OperationKind::TIMER_DELETE:
  case OperationKind::TIMER_SHUTDOWN:
  case OperationKind::TASKLET_KILL:
  case OperationKind::NAPI_DISABLE:
    return LinuxKernelExecutionGraph::EdgeKind::CANCEL;
  default:
    return LinuxKernelExecutionGraph::EdgeKind::REGISTER;
  }
}

bool createsConcurrentExecution(const KernelOperation &op) {
  return op.kind == OperationKind::KTHREAD_START ||
         op.kind == OperationKind::KTHREAD_RUN ||
         op.kind == OperationKind::WORKqueue_SUBMIT ||
         op.kind == OperationKind::TIMER_MOD ||
         op.kind == OperationKind::IRQ_REQUEST ||
         op.kind == OperationKind::RCU_CALL ||
         op.kind == OperationKind::SOFTIRQ_RAISE ||
         op.kind == OperationKind::TASKLET_SCHEDULE ||
         op.kind == OperationKind::NAPI_SCHEDULE;
}

} // namespace

void LinuxKernelExecutionGraph::analyze() {
  contexts_.clear();
  edges_.clear();
  function_contexts_.clear();

  if (process_model_.getConfig().assume_external_entries_parallel) {
    for (const Function &function : process_model_.getModule()) {
      if (function.isDeclaration() || function.hasLocalLinkage()) {
        continue;
      }
      Context context;
      context.id = contexts_.size();
      context.kind = AsyncContextKind::TASK;
      context.entry = &function;
      context.explicit_concurrency = true;
      contexts_.push_back(context);
      function_contexts_[&function].insert(context.id);
    }
  }

  for (const KernelOperation &op : process_model_.getAllOperations()) {
    std::vector<AsyncCallbackRegistration> registrations = op.async_callbacks;
    if (registrations.empty() && op.callback != nullptr) {
      registrations.push_back({op.callback, op.async_context, op.async_object,
                               op.serialization_domain});
    }

    for (const AsyncCallbackRegistration &registration : registrations) {
      const Function *entry = resolveCallback(registration.callback);
      if (entry == nullptr || registration.context == AsyncContextKind::NONE) {
        continue;
      }

      Context context;
      context.id = contexts_.size();
      context.kind = registration.context;
      context.entry = entry;
      context.origin = op.inst;
      context.object = registration.object != nullptr ? registration.object
                                                      : op.async_object;
      context.serialization_domain =
          registration.serialization_domain != nullptr
              ? registration.serialization_domain
              : op.serialization_domain;
      context.serializes_domain =
          registration.serializes_domain || op.serializes_domain;
      context.explicit_concurrency = createsConcurrentExecution(op);
      contexts_.push_back(context);
      function_contexts_[entry].insert(context.id);
      edges_.push_back(
          {edgeKindFor(op), op.inst, context.id, op.is_synchronous});
    }
  }

  auto matchesContext = [&](const KernelOperation &op, const Context &context) {
    if (op.async_object != nullptr && context.object != nullptr &&
        process_model_.mayAlias(op.async_object, context.object)) {
      return true;
    }
    if (op.kind == OperationKind::RCU_BARRIER &&
        context.kind == AsyncContextKind::RCU_CALLBACK) {
      if (op.serialization_domain == nullptr &&
          context.serialization_domain == nullptr) {
        return true;
      }
      return op.serialization_domain != nullptr &&
             context.serialization_domain != nullptr &&
             process_model_.mayAlias(op.serialization_domain,
                                     context.serialization_domain);
    }
    return op.serialization_domain != nullptr &&
           context.serialization_domain != nullptr &&
           process_model_.mayAlias(op.serialization_domain,
                                   context.serialization_domain);
  };
  for (const KernelOperation &op : process_model_.getAllOperations()) {
    const EdgeKind edge_kind = edgeKindFor(op);
    if (edge_kind != EdgeKind::CANCEL && edge_kind != EdgeKind::FLUSH &&
        edge_kind != EdgeKind::JOIN) {
      continue;
    }
    for (const Context &context : contexts_) {
      if (matchesContext(op, context)) {
        edges_.push_back({edge_kind, op.inst, context.id, op.is_synchronous});
      }
    }
  }

  // Propagate callback contexts through the direct call graph.  This gives
  // helpers called from IRQ/work/timer callbacks the context of their caller
  // without treating every address-taken function as an asynchronous entry.
  bool changed = true;
  while (changed) {
    changed = false;
    for (const Function &function : process_model_.getModule()) {
      auto source = function_contexts_.find(&function);
      if (source == function_contexts_.end() || source->second.empty()) {
        continue;
      }
      for (const BasicBlock &block : function) {
        for (const Instruction &instruction : block) {
          const auto *call = dyn_cast<CallBase>(&instruction);
          if (call == nullptr) {
            continue;
          }
          for (const Function *callee :
               process_model_.getPossibleCallees(call)) {
            if (callee == nullptr || callee->isDeclaration()) {
              continue;
            }
            auto &destination = function_contexts_[callee];
            const size_t old_size = destination.size();
            destination.insert(source->second.begin(), source->second.end());
            changed |= destination.size() != old_size;
          }
        }
      }
    }
  }
}

std::vector<const LinuxKernelExecutionGraph::Context *>
LinuxKernelExecutionGraph::getContextsForFunction(
    const Function *function) const {
  std::vector<const Context *> result;
  auto found = function_contexts_.find(function);
  if (found == function_contexts_.end()) {
    return result;
  }
  for (ContextID id : found->second) {
    if (id < contexts_.size()) {
      result.push_back(&contexts_[id]);
    }
  }
  return result;
}

bool LinuxKernelExecutionGraph::originMayPrecede(
    const Instruction *origin, const Instruction *instruction) const {
  if (origin == nullptr || instruction == nullptr ||
      origin->getFunction() != instruction->getFunction()) {
    return false;
  }
  return isPotentiallyReachable(origin, instruction);
}

bool LinuxKernelExecutionGraph::contextsMayOverlap(const Context &lhs,
                                                   const Context &rhs) const {
  if (lhs.id == rhs.id) {
    return false;
  }

  // A single work item or timer instance is not executed concurrently with
  // itself.  Distinct objects on the same general workqueue remain parallel
  // unless a future workqueue-class model proves the queue ordered.
  if (lhs.object != nullptr && lhs.object == rhs.object &&
      lhs.kind == rhs.kind &&
      (lhs.kind == AsyncContextKind::WORKQUEUE ||
       lhs.kind == AsyncContextKind::TIMER_SOFTIRQ)) {
    return false;
  }
  if (lhs.serializes_domain && rhs.serializes_domain &&
      lhs.serialization_domain != nullptr &&
      rhs.serialization_domain != nullptr &&
      process_model_.mayAlias(lhs.serialization_domain,
                              rhs.serialization_domain)) {
    return false;
  }
  return lhs.explicit_concurrency && rhs.explicit_concurrency;
}

bool LinuxKernelExecutionGraph::hasExplicitConcurrencyEvidence(
    const Instruction *lhs, const Instruction *rhs) const {
  if (lhs == nullptr || rhs == nullptr) {
    return false;
  }
  if (lhs == rhs) {
    const auto contexts = getContextsForFunction(lhs->getFunction());
    for (size_t first = 0; first < contexts.size(); ++first) {
      for (size_t second = first + 1; second < contexts.size(); ++second) {
        if (contextsMayOverlap(*contexts[first], *contexts[second])) {
          return true;
        }
      }
    }
    return false;
  }
  if (mhp_ != nullptr && mhp_->mayHappenInParallel(lhs, rhs)) {
    return true;
  }

  const std::vector<const Context *> lhs_contexts =
      getContextsForFunction(lhs->getFunction());
  const std::vector<const Context *> rhs_contexts =
      getContextsForFunction(rhs->getFunction());
  for (const Context *lhs_context : lhs_contexts) {
    for (const Context *rhs_context : rhs_contexts) {
      if (contextsMayOverlap(*lhs_context, *rhs_context)) {
        return true;
      }
    }
  }

  auto maskedOnUniprocessor = [&](const Context &context,
                                  const Instruction *instruction) {
    if (process_model_.getConfig().smp) {
      return false;
    }
    const LinuxKernelProcessModel::ExecutionState state =
        process_model_.getExecutionState(instruction);
    if ((context.kind == AsyncContextKind::HARDIRQ) &&
        state.local_irq_disabled) {
      return true;
    }
    if ((context.kind == AsyncContextKind::SOFTIRQ ||
         context.kind == AsyncContextKind::TASKLET ||
         context.kind == AsyncContextKind::NAPI ||
         context.kind == AsyncContextKind::TIMER_SOFTIRQ ||
         context.kind == AsyncContextKind::RCU_CALLBACK) &&
        state.bh_disabled) {
      return true;
    }
    return (context.kind == AsyncContextKind::KTHREAD ||
            context.kind == AsyncContextKind::WORKQUEUE ||
            context.kind == AsyncContextKind::THREADED_IRQ) &&
           state.preempt_disabled;
  };

  // A submitted callback may overlap the submitting context after the
  // submission point.  Do not infer overlap with operations that can only run
  // before registration/submission.
  for (const Context *context : lhs_contexts) {
    if (context->explicit_concurrency &&
        context->origin->getFunction() == rhs->getFunction() &&
        originMayPrecede(context->origin, rhs) &&
        !maskedOnUniprocessor(*context, rhs)) {
      return true;
    }
  }
  for (const Context *context : rhs_contexts) {
    if (context->explicit_concurrency &&
        context->origin->getFunction() == lhs->getFunction() &&
        originMayPrecede(context->origin, lhs) &&
        !maskedOnUniprocessor(*context, lhs)) {
      return true;
    }
  }
  return false;
}

bool LinuxKernelExecutionGraph::happensBefore(const Instruction *lhs,
                                              const Instruction *rhs) const {
  if (lhs == nullptr || rhs == nullptr || lhs == rhs) {
    return false;
  }
  if (happens_before_ != nullptr && happens_before_->happensBefore(lhs, rhs)) {
    return true;
  }

  if (lhs->getFunction() == rhs->getFunction()) {
    DominatorTree dominators(*const_cast<Function *>(lhs->getFunction()));
    return dominators.dominates(lhs, rhs);
  }

  const auto lhs_contexts = getContextsForFunction(lhs->getFunction());
  for (const Edge &edge : edges_) {
    if (!edge.synchronous || edge.operation == nullptr) {
      continue;
    }
    bool lhs_runs_in_context =
        llvm::any_of(lhs_contexts, [&](const Context *context) {
          return context->id == edge.context;
        });
    if (!lhs_runs_in_context) {
      continue;
    }
    if (edge.operation == rhs) {
      return true;
    }
    if (edge.operation->getFunction() == rhs->getFunction()) {
      DominatorTree dominators(*const_cast<Function *>(rhs->getFunction()));
      if (dominators.dominates(edge.operation, rhs)) {
        return true;
      }
    }
  }

  // Instructions that must precede an asynchronous submission also precede
  // the callback body.
  for (const Context *context : getContextsForFunction(rhs->getFunction())) {
    if (context->origin == nullptr ||
        context->origin->getFunction() != lhs->getFunction()) {
      continue;
    }
    DominatorTree dominators(*const_cast<Function *>(lhs->getFunction()));
    if (dominators.dominates(lhs, context->origin)) {
      return true;
    }
  }
  return false;
}

bool LinuxKernelExecutionGraph::mayHappenInParallel(
    const Instruction *lhs, const Instruction *rhs) const {
  if (!hasExplicitConcurrencyEvidence(lhs, rhs)) {
    return false;
  }
  return !happensBefore(lhs, rhs) && !happensBefore(rhs, lhs);
}

} // namespace kernel
