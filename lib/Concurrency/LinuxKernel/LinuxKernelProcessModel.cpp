/**
 * @file LinuxKernelProcessModel.cpp
 * @brief Linux Kernel Process Model Implementation
 *
 * @author Lotus Analysis Framework
 * @date 2026
 */

#include "Concurrency/LinuxKernel/LinuxKernelProcessModel.h"

#include "Alias/Infrastructure/AliasAnalysisWrapper/AliasAnalysisWrapper.h"

#include <algorithm>
#include <deque>
#include <functional>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>

#include <llvm/ADT/STLExtras.h>
#include <llvm/Analysis/PostDominators.h>
#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Operator.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;

namespace kernel {

namespace {

const Function *resolveDirectCallee(const CallBase *call) {
  if (call == nullptr) {
    return nullptr;
  }

  const Value *called = call->getCalledOperand()->stripPointerCasts();
  if (const auto *function = dyn_cast<Function>(called)) {
    return function;
  }

  const auto *load = dyn_cast<LoadInst>(called);
  if (load == nullptr) {
    return nullptr;
  }
  const Value *pointer = load->getPointerOperand()->stripPointerCasts();
  const auto *global = dyn_cast<GlobalVariable>(pointer);
  if (global == nullptr || !global->isConstant() || !global->hasInitializer()) {
    return nullptr;
  }
  return dyn_cast<Function>(global->getInitializer()->stripPointerCasts());
}

} // namespace

std::vector<const Function *>
LinuxKernelProcessModel::getPossibleCallees(const CallBase *call) const {
  std::vector<const Function *> result;
  if (const Function *direct = resolveDirectCallee(call)) {
    result.push_back(direct);
    return result;
  }
  if (alias_analysis_ == nullptr || !alias_analysis_->isInitialized() ||
      call == nullptr) {
    return result;
  }
  alias_analysis_->getIndirectCallTargets(const_cast<CallBase *>(call), result);
  llvm::sort(result);
  result.erase(std::unique(result.begin(), result.end()), result.end());
  return result;
}

OperationKind
LinuxKernelProcessModel::classifyOperation(const Instruction *inst,
                                           const StringRef &func_name) const {
  (void)inst;
  if (const LinuxKernelAPISemantics *semantics =
          semantic_registry_.lookup(func_name)) {
    return semantics->operation;
  }
  return OperationKind::UNKNOWN;
}

LockKind
LinuxKernelProcessModel::classifyLockKind(const StringRef &func_name) const {
  if (const LinuxKernelAPISemantics *semantics =
          semantic_registry_.lookup(func_name)) {
    return semantics->lock_kind;
  }
  return LockKind::UNKNOWN;
}

void LinuxKernelProcessModel::applyConfiguredSemantics(
    KernelOperation &op, const LinuxKernelAPISemantics &semantics) {
  const auto *call = dyn_cast<CallBase>(op.inst);
  if (call == nullptr) {
    return;
  }
  auto operand = [&](int index) -> const Value * {
    if (index == LinuxKernelAPISemantics::ReturnValue) {
      return call->getType()->isVoidTy() ? nullptr : call;
    }
    if (index < 0 || static_cast<unsigned>(index) >= call->arg_size()) {
      return nullptr;
    }
    return call->getArgOperand(static_cast<unsigned>(index));
  };

  if (const auto *subclass =
          dyn_cast_or_null<ConstantInt>(operand(semantics.subclass_arg))) {
    op.lock_subclass = static_cast<unsigned>(subclass->getZExtValue());
  }

  const Value *object = operand(semantics.object_arg);
  if (object != nullptr) {
    const Value *canonical_object = canonicalizeValue(object);
    switch (op.kind) {
    case OperationKind::LOCK_ACQUIRE:
    case OperationKind::LOCK_RELEASE:
    case OperationKind::LOCK_TRY:
    case OperationKind::LOCK_INIT:
      op.lock = canonical_object;
      op.lock_class = canonicalizeLockClass(
          object,
          semantics.lock_kind != LockKind::UNKNOWN ? semantics.lock_kind
                                                   : op.lock_kind,
          op.lock_subclass);
      break;
    case OperationKind::WAITQUEUE_INIT:
    case OperationKind::WAIT_EVENT:
    case OperationKind::WAKE_UP:
    case OperationKind::PREPARE_WAIT:
    case OperationKind::FINISH_WAIT:
    case OperationKind::COMPLETION_WAIT:
    case OperationKind::COMPLETION_SIGNAL:
    case OperationKind::COMPLETION_INIT:
    case OperationKind::COMPLETION_REINIT:
      op.wait_queue = canonical_object;
      op.async_object = canonical_object;
      break;
    case OperationKind::RCU_ASSIGN:
    case OperationKind::RCU_DEREFERENCE:
    case OperationKind::RCU_CALL:
    case OperationKind::RCU_RECLAIM:
      op.rcu_target = canonical_object;
      op.rcu_sync = canonical_object;
      if (op.kind == OperationKind::RCU_CALL) {
        op.async_object = canonical_object;
      }
      if (op.kind == OperationKind::RCU_RECLAIM) {
        op.memory_object = canonical_object;
      }
      break;
    case OperationKind::KMALLOC:
    case OperationKind::VMALLOC:
    case OperationKind::ALLOC_PAGES:
    case OperationKind::MEMORY_FREE:
      op.memory_object = canonical_object;
      break;
    case OperationKind::ATOMIC_READ:
    case OperationKind::ATOMIC_WRITE:
    case OperationKind::ATOMIC_RMW:
      op.atomic_var = canonical_object;
      break;
    case OperationKind::TIMER_SETUP:
    case OperationKind::TIMER_MOD:
    case OperationKind::TIMER_DELETE:
    case OperationKind::TIMER_SHUTDOWN:
    case OperationKind::KTHREAD_CREATE:
    case OperationKind::KTHREAD_RUN:
    case OperationKind::KTHREAD_START:
    case OperationKind::KTHREAD_STOP:
    case OperationKind::WORKqueue:
    case OperationKind::WORKqueue_CREATE:
    case OperationKind::WORKqueue_SUBMIT:
    case OperationKind::WORKqueue_FLUSH:
    case OperationKind::WORKqueue_CANCEL:
    case OperationKind::WORKqueue_DESTROY:
    case OperationKind::IRQ_REQUEST:
    case OperationKind::IRQ_FREE:
    case OperationKind::IRQ_LINE_ENABLE:
    case OperationKind::IRQ_LINE_DISABLE:
    case OperationKind::TASKLET_SETUP:
    case OperationKind::TASKLET_SCHEDULE:
    case OperationKind::TASKLET_KILL:
    case OperationKind::NAPI_REGISTER:
    case OperationKind::NAPI_SCHEDULE:
    case OperationKind::NAPI_DISABLE:
    case OperationKind::SOFTIRQ_REGISTER:
    case OperationKind::SOFTIRQ_RAISE:
      op.async_object = canonical_object;
      if (op.kind == OperationKind::WORKqueue ||
          op.kind == OperationKind::WORKqueue_SUBMIT ||
          op.kind == OperationKind::TIMER_SETUP ||
          op.kind == OperationKind::TIMER_MOD ||
          op.kind == OperationKind::TIMER_DELETE ||
          op.kind == OperationKind::TIMER_SHUTDOWN) {
        op.wait_queue = canonical_object;
      }
      break;
    default:
      break;
    }
  }

  if (const Value *domain = operand(semantics.domain_arg)) {
    op.serialization_domain = canonicalizeValue(domain);
    if (op.kind == OperationKind::RCU_READ_LOCK ||
        op.kind == OperationKind::RCU_READ_UNLOCK ||
        op.kind == OperationKind::RCU_SYNC ||
        op.kind == OperationKind::RCU_CALL ||
        op.kind == OperationKind::RCU_BARRIER) {
      op.rcu_domain = op.serialization_domain;
    }
  }
  if (const Value *condition = operand(semantics.condition_arg)) {
    op.wait_condition = condition;
  }
  if (const Value *flags = operand(semantics.flags_arg)) {
    op.irq_flags = canonicalizeValue(flags);
  }
  if (const Value *expires = operand(semantics.expires_arg)) {
    op.timer_expires = expires;
  }
  if (const Value *size = operand(semantics.size_arg)) {
    op.allocation_size = size;
  }
  if (const auto *value =
          dyn_cast_or_null<ConstantInt>(operand(semantics.value_arg))) {
    if (value->getSExtValue() <= std::numeric_limits<int>::max() &&
        value->getSExtValue() >= std::numeric_limits<int>::min()) {
      op.atomic_value = static_cast<int>(value->getSExtValue());
    }
  }

  auto registerCallback = [&](const Value *callback,
                              AsyncContextKind context) {
    if (callback == nullptr) {
      return;
    }
    callback = callback->stripPointerCasts();
    if (!isa<ConstantPointerNull>(callback)) {
      if (op.callback == nullptr) {
        op.callback = callback;
      }
      if (!llvm::is_contained(op.callbacks, callback)) {
        op.callbacks.push_back(callback);
      }
      if (op.async_context == AsyncContextKind::NONE) {
        op.async_context = context;
      }
      const bool already_registered =
          llvm::any_of(op.async_callbacks,
                       [&](const AsyncCallbackRegistration &registration) {
                         return registration.callback == callback &&
                                registration.context == context;
                       });
      if (!already_registered && context != AsyncContextKind::NONE) {
        op.async_callbacks.push_back({callback, context, op.async_object,
                                      op.serialization_domain,
                                      semantics.serializes_domain});
      }
    }
  };
  registerCallback(operand(semantics.callback_arg), semantics.async_context);
  registerCallback(operand(semantics.secondary_callback_arg),
                   semantics.secondary_async_context);
  if (op.callback == nullptr &&
      semantics.async_context != AsyncContextKind::NONE) {
    op.async_context = semantics.async_context;
  }

  if (semantics.lock_kind != LockKind::UNKNOWN) {
    op.lock_kind = semantics.lock_kind;
  }
  if (semantics.lock_mode != LockMode::UNKNOWN) {
    op.lock_mode = semantics.lock_mode;
    op.reader_kind =
        semantics.lock_mode == LockMode::SHARED
            ? (op.lock_kind == LockKind::RWLOCK ? LockReaderKind::RECURSIVE
                                                : LockReaderKind::NON_RECURSIVE)
            : LockReaderKind::NONE;
    op.is_recursive = op.reader_kind == LockReaderKind::RECURSIVE;
  }
  op.conditional_success = semantics.success;
  op.rcu_flavor = semantics.rcu_flavor;
  op.completion_signal = semantics.completion_signal;
  op.memory_order = semantics.memory_order;
  op.is_synchronous |= semantics.synchronous;
  op.serializes_domain |= semantics.serializes_domain;
  op.is_raw |= semantics.raw_lock;
  op.is_nested |= semantics.nested_lock;
  op.is_interruptible |= semantics.interruptible;
  op.has_timeout |= semantics.timeout;
  op.wake_all |= semantics.wake_all;
  op.wake_exclusive |= semantics.wake_exclusive;
  op.deferred_reclamation |= semantics.deferred_reclamation;
  op.returns_retired_pointer |= semantics.returns_retired_pointer;
  op.requires_rcu_section = semantics.requires_rcu_section;
  op.managed_allocation |= semantics.managed_allocation;
  op.may_sleep |= semantics.may_sleep;
  op.may_spawn |= semantics.may_spawn;
  op.may_access_shared_memory |= semantics.may_access_shared_memory;
  op.saves_irq_state |= semantics.saves_irq_state;
  op.restores_irq_state |= semantics.restores_irq_state;
  op.disables_local_irq |= semantics.disables_local_irq;
  op.enables_local_irq |= semantics.enables_local_irq;
  op.disables_bh |= semantics.disables_bh;
  op.enables_bh |= semantics.enables_bh;
  if (!semantics.preemption_effect_non_rt || !config_.isPreemptRT()) {
    op.disables_preemption |= semantics.disables_preemption;
    op.enables_preemption |= semantics.enables_preemption;
  }
}

void LinuxKernelProcessModel::trackLockState(KernelOperation &op) {
  if ((op.kind == OperationKind::LOCK_ACQUIRE ||
       op.kind == OperationKind::LOCK_TRY) &&
      op.lock != nullptr) {
    auto &lock_info = lock_info_map_[op.lock];
    lock_info.id = op.lock;
    lock_info.lock_class = op.lock_class;
    lock_info.kind = op.lock_kind;
    lock_info.acquire_inst = op.inst;
    lock_info.acquire_history.push_back(op.inst);
    lock_info.acquire_count++;

    if (op.is_recursive) {
      lock_info.is_recursive = true;
    }
    if (op.is_nested) {
      lock_info.is_nested = true;
    }
    if (op.is_interruptible) {
      lock_info.is_interruptible = true;
    }
    if (op.is_raw) {
      lock_info.is_raw = true;
    }

    auto key = std::make_pair(op.inst->getFunction(), op.lock);
    int &depth = lock_depth_[key];
    depth = std::max(depth + 1, 1);
  }

  if (op.kind == OperationKind::LOCK_RELEASE && op.lock != nullptr) {
    auto &lock_info = lock_info_map_[op.lock];
    lock_info.id = op.lock;
    if (!lock_info.lock_class.isValid()) {
      lock_info.lock_class = op.lock_class;
    }
    if (lock_info.kind == LockKind::UNKNOWN) {
      lock_info.kind = op.lock_kind;
    }
    lock_info.release_inst = op.inst;
    lock_info.release_history.push_back(op.inst);
    lock_info.release_count++;

    auto key = std::make_pair(op.inst->getFunction(), op.lock);
    int &depth = lock_depth_[key];
    depth = std::max(depth - 1, 0);
  }

  if (op.kind == OperationKind::LOCK_INIT && op.lock != nullptr) {
    auto &lock_info = lock_info_map_[op.lock];
    lock_info.id = op.lock;
    lock_info.lock_class = op.lock_class;
    lock_info.kind = op.lock_kind;
    lock_info.init_inst = op.inst;
  }
}

void LinuxKernelProcessModel::analyzeLockUsage() {}

void LinuxKernelProcessModel::analyzeModule() {
  semantic_registry_.load(config_);
  all_operations_.clear();
  operation_kind_counts_.clear();
  lock_info_map_.clear();
  rcu_sections_.clear();
  wait_queue_entries_.clear();
  lock_depth_.clear();
  operations_by_function_.clear();
  operation_indices_by_inst_.clear();
  instruction_order_.clear();
  canonical_pointer_ids_.clear();

  struct LockEffectSummary {
    OperationKind kind = OperationKind::UNKNOWN;
    LockKind lock_kind = LockKind::UNKNOWN;
    std::string semantic_name;
    unsigned object_arg_index = 0;
    LinuxKernelAPISemantics semantics;
  };
  using LockWrapperSummary = std::vector<LockEffectSummary>;
  std::map<const Function *, LockWrapperSummary> lock_wrapper_summaries;
  std::map<const Value *, const Value *> registered_work_callbacks;
  std::map<const Value *, const Value *> registered_timer_callbacks;
  std::map<const Value *, const Value *> registered_kthread_callbacks;
  std::map<const Value *, const Value *> registered_tasklet_callbacks;
  std::map<const Value *, const Value *> registered_napi_callbacks;
  std::map<const Value *, const Value *> registered_softirq_callbacks;
  std::set<const Value *> ordered_workqueue_domains;

  auto traceArgument = [&](const Function &function,
                           const Value *value) -> std::optional<unsigned> {
    if (value == nullptr) {
      return std::nullopt;
    }
    value = value->stripPointerCasts();
    if (const auto *argument = dyn_cast<Argument>(value)) {
      if (argument->getParent() == &function) {
        return argument->getArgNo();
      }
      return std::nullopt;
    }

    // Zero-offset projections are representational casts and can be safely
    // instantiated with the caller's argument.  Non-zero field projections
    // require the richer memory-object path representation and are therefore
    // intentionally not summarized here.
    const DataLayout &layout = module_.getDataLayout();
    APInt offset(layout.getIndexTypeSizeInBits(value->getType()), 0, true);
    const Value *base = value->stripAndAccumulateConstantOffsets(
        layout, offset, /*AllowNonInbounds=*/true);
    const auto *argument = dyn_cast_or_null<Argument>(base);
    if (argument != nullptr && argument->getParent() == &function &&
        offset.isZero()) {
      return argument->getArgNo();
    }
    return std::nullopt;
  };

  // Compute compositional lock summaries to a fixed point.  A wrapper may
  // contain multiple lock operations and may call another summarized wrapper;
  // effects remain ordered and are instantiated at each callsite.
  bool summary_progress = true;
  while (summary_progress) {
    summary_progress = false;
    for (const Function &function : module_) {
      if (function.isDeclaration() ||
          lock_wrapper_summaries.count(&function) > 0) {
        continue;
      }

      LockWrapperSummary summary;
      bool valid = true;
      for (const Instruction &instruction : instructions(function)) {
        const auto *call = dyn_cast<CallBase>(&instruction);
        if (call == nullptr) {
          continue;
        }
        const std::vector<const Function *> callees = getPossibleCallees(call);
        if (callees.size() != 1) {
          valid = false;
          break;
        }
        const Function *callee = callees.front();
        if (callee->isIntrinsic()) {
          continue;
        }

        OperationKind kind = classifyOperation(&instruction, callee->getName());
        if (kind == OperationKind::LOCK_ACQUIRE ||
            kind == OperationKind::LOCK_RELEASE ||
            kind == OperationKind::LOCK_TRY) {
          const LinuxKernelAPISemantics *semantics =
              semantic_registry_.lookup(callee->getName());
          if (semantics == nullptr || semantics->object_arg < 0) {
            valid = false;
            break;
          }
          const unsigned object_index =
              static_cast<unsigned>(semantics->object_arg);
          if (call->arg_size() <= object_index) {
            valid = false;
            break;
          }
          std::optional<unsigned> argument =
              traceArgument(function, call->getArgOperand(object_index));
          if (!argument.has_value()) {
            valid = false;
            break;
          }
          summary.push_back({kind, classifyLockKind(callee->getName()),
                             callee->getName().str(), *argument, *semantics});
          continue;
        }

        auto nested = lock_wrapper_summaries.find(callee);
        if (nested == lock_wrapper_summaries.end()) {
          valid = false;
          break;
        }
        for (const LockEffectSummary &effect : nested->second) {
          if (call->arg_size() <= effect.object_arg_index) {
            valid = false;
            break;
          }
          std::optional<unsigned> argument = traceArgument(
              function, call->getArgOperand(effect.object_arg_index));
          if (!argument.has_value()) {
            valid = false;
            break;
          }
          LockEffectSummary instantiated = effect;
          instantiated.object_arg_index = *argument;
          summary.push_back(std::move(instantiated));
        }
        if (!valid) {
          break;
        }
      }
      if (valid && !summary.empty()) {
        lock_wrapper_summaries[&function] = std::move(summary);
        summary_progress = true;
      }
    }
  }

  size_t instruction_order = 0;

  for (Function &F : module_) {
    if (lock_wrapper_summaries.count(&F) > 0) {
      continue;
    }
    for (inst_iterator II = inst_begin(F), E = inst_end(F); II != E; ++II) {
      Instruction *I = &*II;
      instruction_order_[I] = instruction_order++;

      const CallBase *cb = dyn_cast<CallBase>(I);
      if (!cb) {
        continue;
      }

      const std::vector<const Function *> callees = getPossibleCallees(cb);
      if (callees.size() != 1) {
        KernelOperation op(I, OperationKind::UNKNOWN_CALL);
        op.function_name = "<unresolved-indirect-call>";
        op.has_unknown_effects = true;
        op.may_sleep = true;
        op.may_spawn = true;
        op.may_access_shared_memory = true;
        const size_t index = all_operations_.size();
        all_operations_.push_back(op);
        operation_indices_by_inst_[I].push_back(index);
        operations_by_function_[I->getFunction()].push_back(index);
        operation_kind_counts_[op.kind]++;
        continue;
      }
      const Function *callee = callees.front();
      if (callee->isIntrinsic()) {
        continue;
      }

      auto wrapper = lock_wrapper_summaries.find(callee);
      if (wrapper != lock_wrapper_summaries.end()) {
        for (const LockEffectSummary &effect : wrapper->second) {
          if (cb->arg_size() <= effect.object_arg_index) {
            continue;
          }
          KernelOperation op(I, effect.kind, effect.lock_kind);
          op.function_name = effect.semantic_name;
          LinuxKernelAPISemantics semantics = effect.semantics;
          semantics.object_arg = effect.object_arg_index;
          applyConfiguredSemantics(op, semantics);
          trackLockState(op);

          const size_t index = all_operations_.size();
          all_operations_.push_back(op);
          operation_indices_by_inst_[I].push_back(index);
          operations_by_function_[I->getFunction()].push_back(index);
          operation_kind_counts_[effect.kind]++;
        }
        continue;
      }

      StringRef func_name = callee->getName();
      const LinuxKernelAPISemantics *configured_semantics =
          semantic_registry_.lookup(func_name);
      OperationKind kind = OperationKind::UNKNOWN;
      LockKind lock_kind = LockKind::UNKNOWN;
      kind = classifyOperation(I, func_name);
      lock_kind = classifyLockKind(func_name);
      if (kind == OperationKind::UNKNOWN) {
        kind = OperationKind::UNKNOWN_CALL;
      }

      KernelOperation op(I, kind, lock_kind);
      op.function_name = func_name.str();

      if (kind == OperationKind::UNKNOWN_CALL) {
        op.has_unknown_effects = true;
        op.may_sleep = true;
        op.may_spawn = true;
        op.may_access_shared_memory = true;
      }

      if (configured_semantics != nullptr) {
        applyConfiguredSemantics(op, *configured_semantics);
      }
      if (kind == OperationKind::LOCK_ACQUIRE ||
          kind == OperationKind::LOCK_RELEASE ||
          kind == OperationKind::LOCK_TRY || kind == OperationKind::LOCK_INIT) {
        trackLockState(op);
      }

      const size_t index = all_operations_.size();
      all_operations_.push_back(op);
      operation_indices_by_inst_[I].push_back(index);
      operations_by_function_[I->getFunction()].push_back(index);
      operation_kind_counts_[kind]++;
    }
  }

  registered_work_callbacks.clear();
  registered_timer_callbacks.clear();
  registered_kthread_callbacks.clear();
  registered_tasklet_callbacks.clear();
  registered_napi_callbacks.clear();
  registered_softirq_callbacks.clear();
  ordered_workqueue_domains.clear();
  for (const KernelOperation &op : all_operations_) {
    if (op.kind == OperationKind::WORKqueue && op.wait_queue != nullptr &&
        op.callback != nullptr) {
      registered_work_callbacks[op.wait_queue] = op.callback;
    }
    if (op.kind == OperationKind::TIMER_SETUP && op.async_object != nullptr &&
        op.callback != nullptr) {
      registered_timer_callbacks[op.async_object] = op.callback;
    }
    if (op.kind == OperationKind::KTHREAD_CREATE &&
        op.async_object != nullptr && op.callback != nullptr) {
      registered_kthread_callbacks[op.async_object] = op.callback;
    }
    if (op.kind == OperationKind::TASKLET_SETUP && op.async_object != nullptr &&
        op.callback != nullptr) {
      registered_tasklet_callbacks[op.async_object] = op.callback;
    }
    if (op.kind == OperationKind::NAPI_REGISTER && op.async_object != nullptr &&
        op.callback != nullptr) {
      registered_napi_callbacks[op.async_object] = op.callback;
    }
    if (op.kind == OperationKind::SOFTIRQ_REGISTER &&
        op.async_object != nullptr && op.callback != nullptr) {
      registered_softirq_callbacks[op.async_object] = op.callback;
    }
    if (op.kind == OperationKind::WORKqueue_CREATE && op.serializes_domain &&
        op.serialization_domain != nullptr) {
      ordered_workqueue_domains.insert(op.serialization_domain);
    }
  }
  for (KernelOperation &op : all_operations_) {
    if (op.kind == OperationKind::WORKqueue_SUBMIT &&
        ordered_workqueue_domains.count(op.serialization_domain) > 0) {
      op.serializes_domain = true;
      for (AsyncCallbackRegistration &registration : op.async_callbacks) {
        registration.serializes_domain = true;
      }
    }
    if (op.kind == OperationKind::WORKqueue_SUBMIT && op.callback == nullptr) {
      auto callback = registered_work_callbacks.find(op.wait_queue);
      if (callback != registered_work_callbacks.end()) {
        op.callback = callback->second;
        op.callbacks.push_back(op.callback);
        op.async_callbacks.push_back({op.callback, AsyncContextKind::WORKQUEUE,
                                      op.async_object,
                                      op.serialization_domain});
      }
    }
    if (op.kind == OperationKind::TIMER_MOD && op.callback == nullptr) {
      auto callback = registered_timer_callbacks.find(op.async_object);
      if (callback != registered_timer_callbacks.end()) {
        op.callback = callback->second;
        op.callbacks.push_back(op.callback);
        op.async_context = AsyncContextKind::TIMER_SOFTIRQ;
        op.serialization_domain = op.async_object;
        op.async_callbacks.push_back({op.callback, op.async_context,
                                      op.async_object,
                                      op.serialization_domain});
      }
    }
    if (op.kind == OperationKind::KTHREAD_START && op.callback == nullptr) {
      auto callback = registered_kthread_callbacks.find(op.async_object);
      if (callback != registered_kthread_callbacks.end()) {
        op.callback = callback->second;
        op.callbacks.push_back(op.callback);
        op.async_context = AsyncContextKind::KTHREAD;
        op.async_callbacks.push_back(
            {op.callback, op.async_context, op.async_object, nullptr});
      }
    }
    auto attachCallback = [&](const std::map<const Value *, const Value *> &map,
                              AsyncContextKind context, bool serializes) {
      auto callback = map.find(op.async_object);
      if (callback == map.end()) {
        return;
      }
      op.callback = callback->second;
      op.callbacks.push_back(op.callback);
      op.async_context = context;
      op.serializes_domain = serializes;
      op.async_callbacks.push_back({op.callback, context, op.async_object,
                                    op.serialization_domain, serializes});
    };
    if (op.kind == OperationKind::TASKLET_SCHEDULE && op.callback == nullptr) {
      attachCallback(registered_tasklet_callbacks, AsyncContextKind::TASKLET,
                     true);
    }
    if (op.kind == OperationKind::NAPI_SCHEDULE && op.callback == nullptr) {
      attachCallback(registered_napi_callbacks, AsyncContextKind::NAPI, true);
    }
    if (op.kind == OperationKind::SOFTIRQ_RAISE && op.callback == nullptr) {
      attachCallback(registered_softirq_callbacks, AsyncContextKind::SOFTIRQ,
                     false);
    }
  }

  analyzeLockUsage();
}

std::vector<KernelOperation>
LinuxKernelProcessModel::getOperationsByKind(OperationKind kind) const {
  std::vector<KernelOperation> result;
  for (const KernelOperation &op : all_operations_) {
    if (op.kind == kind) {
      result.push_back(op);
    }
  }
  return result;
}

std::vector<KernelOperation>
LinuxKernelProcessModel::getOperationsByLock(LockID lock) const {
  std::vector<KernelOperation> result;
  for (const KernelOperation &op : all_operations_) {
    if (op.lock == lock) {
      result.push_back(op);
    }
  }
  return result;
}

std::vector<const KernelOperation *>
LinuxKernelProcessModel::getOperationsInFunction(
    const Function *function) const {
  std::vector<const KernelOperation *> result;
  auto it = operations_by_function_.find(function);
  if (it == operations_by_function_.end()) {
    return result;
  }

  result.reserve(it->second.size());
  for (size_t index : it->second) {
    result.push_back(&all_operations_[index]);
  }

  return result;
}

const KernelOperation *LinuxKernelProcessModel::getOperationForInstruction(
    const Instruction *inst) const {
  auto it = operation_indices_by_inst_.find(inst);
  if (it == operation_indices_by_inst_.end() || it->second.empty()) {
    return nullptr;
  }

  return &all_operations_[it->second.front()];
}

std::vector<const KernelOperation *>
LinuxKernelProcessModel::getOperationsForInstruction(
    const Instruction *inst) const {
  std::vector<const KernelOperation *> result;
  auto found = operation_indices_by_inst_.find(inst);
  if (found == operation_indices_by_inst_.end()) {
    return result;
  }
  result.reserve(found->second.size());
  for (size_t index : found->second) {
    result.push_back(&all_operations_[index]);
  }
  return result;
}

bool LinuxKernelProcessModel::isBeforeInFunction(const Instruction *lhs,
                                                 const Instruction *rhs) const {
  if (lhs == nullptr || rhs == nullptr ||
      lhs->getFunction() != rhs->getFunction()) {
    return false;
  }

  auto lhs_it = instruction_order_.find(lhs);
  auto rhs_it = instruction_order_.find(rhs);
  if (lhs_it == instruction_order_.end() ||
      rhs_it == instruction_order_.end()) {
    return false;
  }

  return lhs_it->second < rhs_it->second;
}

const Value *
LinuxKernelProcessModel::canonicalizeValue(const Value *value) const {
  if (value == nullptr) {
    return nullptr;
  }

  const Value *stripped = value->stripPointerCasts();
  if (!stripped->getType()->isPointerTy()) {
    return stripped;
  }

  const DataLayout &layout = module_.getDataLayout();
  APInt offset(layout.getIndexTypeSizeInBits(stripped->getType()), 0, true);
  const Value *base = stripped->stripAndAccumulateConstantOffsets(
      layout, offset, /*AllowNonInbounds=*/true);
  if (base == nullptr) {
    return stripped;
  }

  auto key = std::make_pair(base, offset.getSExtValue());
  auto [identity, inserted] = canonical_pointer_ids_.emplace(key, stripped);
  return identity->second;
}

LockClassID LinuxKernelProcessModel::canonicalizeLockClass(
    const Value *value, LockKind kind, unsigned subclass) const {
  LockClassID result;
  result.kind = kind;
  result.subclass = subclass;
  if (value == nullptr) {
    return result;
  }

  const Value *stripped = value->stripPointerCasts();
  if (!stripped->getType()->isPointerTy()) {
    return result;
  }

  // Preserve the aggregate type used by the GEP.  For argument-based object
  // instances this lets the same struct field share a lock class even though
  // the concrete SSA bases differ between callers.
  for (const Value *cursor = stripped;;) {
    const auto *gep = dyn_cast<GEPOperator>(cursor);
    if (gep == nullptr) {
      break;
    }
    result.aggregate_type = gep->getSourceElementType();
    cursor = gep->getPointerOperand()->stripPointerCasts();
  }

  const DataLayout &layout = module_.getDataLayout();
  APInt offset(layout.getIndexTypeSizeInBits(stripped->getType()), 0, true);
  const Value *base = stripped->stripAndAccumulateConstantOffsets(
      layout, offset, /*AllowNonInbounds=*/true);
  if (base == nullptr) {
    return result;
  }
  result.byte_offset = offset.getSExtValue();
  result.precise = !isa<GEPOperator>(base);

  if (isa<GlobalVariable>(base) || isa<AllocaInst>(base) ||
      isa<CallBase>(base)) {
    result.static_key = base;
  }
  if (result.aggregate_type == nullptr && base->getType()->isPointerTy()) {
    result.aggregate_type = base->getType()->getPointerElementType();
  }
  return result;
}

bool LinuxKernelProcessModel::mayAlias(const Value *lhs,
                                       const Value *rhs) const {
  if (lhs == nullptr || rhs == nullptr) {
    return false;
  }
  if (canonicalizeValue(lhs) == canonicalizeValue(rhs)) {
    return true;
  }
  if (alias_analysis_ != nullptr && alias_analysis_->isInitialized()) {
    return alias_analysis_->mayAlias(lhs, rhs);
  }
  return false;
}

bool LinuxKernelProcessModel::mustAlias(const Value *lhs,
                                        const Value *rhs) const {
  if (lhs == nullptr || rhs == nullptr) {
    return false;
  }
  if (canonicalizeValue(lhs) == canonicalizeValue(rhs)) {
    return true;
  }
  return alias_analysis_ != nullptr && alias_analysis_->isInitialized() &&
         alias_analysis_->mustAlias(lhs, rhs);
}

bool LinuxKernelProcessModel::getAliasSet(
    const Value *value, std::vector<const Value *> &aliases) const {
  aliases.clear();
  if (value == nullptr || alias_analysis_ == nullptr ||
      !alias_analysis_->isInitialized()) {
    return false;
  }
  return alias_analysis_->getAliasSet(value, aliases);
}

std::vector<KernelOperation>
LinuxKernelProcessModel::findLockAcquiresWithoutRelease() const {
  // Function-global operation counts cannot establish a path-specific leak.
  // LinuxKernelLockAnalysis owns the CFG-aware query.
  return {};
}

std::vector<std::pair<const Instruction *, const Instruction *>>
LinuxKernelProcessModel::findPotentialDeadlocks() const {
  std::vector<std::pair<const Instruction *, const Instruction *>> deadlocks;
  return deadlocks;
}

std::vector<const Instruction *>
LinuxKernelProcessModel::findDoubleLocks() const {
  return {};
}

std::vector<const Instruction *>
LinuxKernelProcessModel::findUnlockWithoutLock() const {
  return {};
}

std::vector<const Instruction *>
LinuxKernelProcessModel::findMixRawAndcooked() const {
  std::vector<const Instruction *> result;
  std::map<LockID, bool> saw_raw;
  std::map<LockID, bool> saw_cooked;

  for (const auto &op : all_operations_) {
    if (op.lock == nullptr || (op.kind != OperationKind::LOCK_ACQUIRE &&
                               op.kind != OperationKind::LOCK_RELEASE &&
                               op.kind != OperationKind::LOCK_TRY)) {
      continue;
    }
    saw_raw[op.lock] = saw_raw[op.lock] || op.is_raw;
    saw_cooked[op.lock] = saw_cooked[op.lock] || !op.is_raw;
  }

  for (const auto &op : all_operations_) {
    if (op.lock != nullptr && saw_raw[op.lock] && saw_cooked[op.lock]) {
      result.push_back(op.inst);
    }
  }
  return result;
}

std::vector<std::pair<const Instruction *, const Instruction *>>
LinuxKernelProcessModel::findLockOrderInversion() const {
  std::vector<std::pair<const Instruction *, const Instruction *>> inversions;
  return inversions;
}

std::vector<const Instruction *>
LinuxKernelProcessModel::findRCUWithoutGracePeriod() const {
  return {};
}

std::vector<const Instruction *>
LinuxKernelProcessModel::findSleepInAtomic() const {
  std::vector<const Instruction *> result;
  for (const auto &op : all_operations_) {
    if (maySleep(op.inst) && isInAtomicContext(op.inst)) {
      result.push_back(op.inst);
    }
  }
  return result;
}

std::vector<std::pair<const Instruction *, const Instruction *>>
LinuxKernelProcessModel::findUseAfterFree() const {
  std::vector<std::pair<const Instruction *, const Instruction *>> result;
  return result;
}

std::vector<const Instruction *>
LinuxKernelProcessModel::findTimerIssues() const {
  std::vector<const Instruction *> result;
  std::map<const Function *, std::unique_ptr<DominatorTree>> dominators;
  for (const auto &mod : all_operations_) {
    if (mod.kind != OperationKind::TIMER_MOD || mod.wait_queue == nullptr) {
      continue;
    }
    const Function *function = mod.inst->getFunction();
    auto &dt = dominators[function];
    if (!dt) {
      dt = std::make_unique<DominatorTree>(*const_cast<Function *>(function));
    }
    for (const auto &shutdown : all_operations_) {
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

std::vector<const Instruction *>
LinuxKernelProcessModel::findIrqSaveRestoreMismatch() const {
  std::vector<const Instruction *> result;
  auto isSave = [](const KernelOperation &op) {
    return op.saves_irq_state;
  };
  auto isRestore = [](const KernelOperation &op) {
    return op.restores_irq_state;
  };
  auto flagsMatch = [](const KernelOperation &save,
                       const KernelOperation &restore) {
    return save.irq_flags == nullptr || restore.irq_flags == nullptr ||
           save.irq_flags == restore.irq_flags;
  };

  for (const Function &function : module_) {
    if (function.isDeclaration()) {
      continue;
    }
    DominatorTree dominators(*const_cast<Function *>(&function));
    PostDominatorTree post_dominators;
    post_dominators.recalculate(*const_cast<Function *>(&function));

    for (const auto &restore : all_operations_) {
      if (restore.inst->getFunction() != &function || !isRestore(restore)) {
        continue;
      }
      bool matched =
          llvm::any_of(all_operations_, [&](const KernelOperation &save) {
            return save.inst->getFunction() == &function && isSave(save) &&
                   flagsMatch(save, restore) &&
                   dominators.dominates(save.inst, restore.inst);
          });
      if (!matched) {
        result.push_back(restore.inst);
      }
    }

    for (const auto &save : all_operations_) {
      if (save.inst->getFunction() != &function || !isSave(save)) {
        continue;
      }
      bool matched =
          llvm::any_of(all_operations_, [&](const KernelOperation &restore) {
            return restore.inst->getFunction() == &function &&
                   isRestore(restore) && flagsMatch(save, restore) &&
                   post_dominators.dominates(restore.inst, save.inst);
          });
      if (!matched) {
        result.push_back(save.inst);
      }
    }
  }
  return result;
}

LinuxKernelProcessModel::ExecutionState
LinuxKernelProcessModel::getExecutionState(const Instruction *inst) const {
  if (inst == nullptr) {
    return {};
  }

  struct AtomicState {
    bool irq_disabled = false;
    bool bh_disabled = false;
    bool preempt_disabled = false;

    bool operator==(const AtomicState &other) const {
      return irq_disabled == other.irq_disabled &&
             bh_disabled == other.bh_disabled &&
             preempt_disabled == other.preempt_disabled;
    }
  };

  auto transfer = [](AtomicState &state, const KernelOperation *op) {
    if (op == nullptr) {
      return;
    }
    if (op->disables_local_irq || op->saves_irq_state ||
        op->kind == OperationKind::IRQ_DISABLE) {
      state.irq_disabled = true;
    } else if (op->enables_local_irq || op->restores_irq_state ||
               op->kind == OperationKind::IRQ_ENABLE) {
      state.irq_disabled = false;
    }
    if (op->disables_bh || op->kind == OperationKind::BH_DISABLE) {
      state.bh_disabled = true;
    } else if (op->enables_bh || op->kind == OperationKind::BH_ENABLE) {
      state.bh_disabled = false;
    }
    if (op->disables_preemption ||
        op->kind == OperationKind::PREEMPT_DISABLE) {
      state.preempt_disabled = true;
    } else if (op->enables_preemption ||
               op->kind == OperationKind::PREEMPT_ENABLE) {
      state.preempt_disabled = false;
    }
  };

  const Function *function = inst->getFunction();
  std::map<const BasicBlock *, AtomicState> in_states;
  std::set<const BasicBlock *> initialized;
  std::deque<const BasicBlock *> worklist;
  const BasicBlock *entry = &function->getEntryBlock();
  initialized.insert(entry);
  worklist.push_back(entry);

  while (!worklist.empty()) {
    const BasicBlock *block = worklist.front();
    worklist.pop_front();
    AtomicState state = in_states[block];
    for (const Instruction &instruction : *block) {
      for (const KernelOperation *op :
           getOperationsForInstruction(&instruction)) {
        transfer(state, op);
      }
    }
    for (const BasicBlock *successor : successors(block)) {
      if (initialized.insert(successor).second) {
        in_states[successor] = state;
        worklist.push_back(successor);
        continue;
      }
      AtomicState merged = in_states[successor];
      merged.irq_disabled |= state.irq_disabled;
      merged.bh_disabled |= state.bh_disabled;
      merged.preempt_disabled |= state.preempt_disabled;
      if (!(merged == in_states[successor])) {
        in_states[successor] = merged;
        worklist.push_back(successor);
      }
    }
  }

  AtomicState state = in_states[inst->getParent()];
  for (const Instruction &instruction : *inst->getParent()) {
    if (&instruction == inst) {
      break;
    }
    for (const KernelOperation *op :
         getOperationsForInstruction(&instruction)) {
      transfer(state, op);
    }
  }
  return {state.irq_disabled, state.bh_disabled, state.preempt_disabled};
}

bool LinuxKernelProcessModel::isInAtomicContext(const Instruction *inst) const {
  const ExecutionState state = getExecutionState(inst);
  return state.local_irq_disabled || state.bh_disabled ||
         state.preempt_disabled;
}

bool LinuxKernelProcessModel::maySleep(const Instruction *inst) const {
  const CallBase *cb = dyn_cast<CallBase>(inst);
  if (!cb) {
    return false;
  }

  const std::vector<const Function *> callees = getPossibleCallees(cb);
  if (callees.size() != 1) {
    return false;
  }
  const Function *callee = callees.front();
  const LinuxKernelAPISemantics *semantics =
      semantic_registry_.lookup(callee->getName());
  return semantics != nullptr && semantics->may_sleep;
}

} // namespace kernel
