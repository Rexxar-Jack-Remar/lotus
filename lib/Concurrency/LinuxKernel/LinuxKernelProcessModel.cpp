/**
 * @file LinuxKernelProcessModel.cpp
 * @brief Linux Kernel Process Model Implementation
 *
 * @author Lotus Analysis Framework
 * @date 2026
 */

#include "Concurrency/LinuxKernel/LinuxKernelProcessModel.h"

#include "Concurrency/Utils/LinuxKernel.h"

#include <algorithm>
#include <deque>
#include <functional>
#include <limits>
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

bool isTimerSetupCall(StringRef func_name) {
  return func_name.equals("timer_setup") || func_name.equals("setup_timer") ||
         func_name.equals("init_timer_key") || func_name.equals("init_timer") ||
         func_name.equals("__timer_init") ||
         func_name.equals("timer_setup_key");
}

bool isTimerModCall(StringRef func_name) {
  return func_name.equals("mod_timer") || func_name.equals("add_timer") ||
         func_name.equals("add_timer_on");
}

bool isTimerDeleteCall(StringRef func_name) {
  return func_name.equals("del_timer") || func_name.equals("del_timer_sync") ||
         func_name.equals("timer_delete") ||
         func_name.equals("timer_delete_sync");
}

bool isTimerShutdownCall(StringRef func_name) {
  return func_name.equals("timer_shutdown") ||
         func_name.equals("timer_shutdown_sync");
}

bool isLocalIrqDisableCall(StringRef func_name) {
  return func_name.equals("local_irq_disable") ||
         func_name.equals("local_irq_save") ||
         func_name.equals("arch_local_irq_disable") ||
         func_name.equals("arch_local_irq_save");
}

bool isLocalIrqEnableCall(StringRef func_name) {
  return func_name.equals("local_irq_enable") ||
         func_name.equals("local_irq_restore") ||
         func_name.equals("arch_local_irq_enable") ||
         func_name.equals("arch_local_irq_restore");
}

bool isLineIrqDisableCall(StringRef func_name) {
  return func_name.equals("disable_irq") ||
         func_name.equals("disable_irq_nosync");
}

bool isLineIrqEnableCall(StringRef func_name) {
  return func_name.equals("enable_irq");
}

bool isIrqRequestCall(StringRef func_name) {
  return func_name.equals("request_irq") ||
         func_name.equals("request_threaded_irq");
}

bool isIrqFreeCall(StringRef func_name) { return func_name.equals("free_irq"); }

bool isBhDisableCall(StringRef func_name) {
  return func_name.equals("local_bh_disable") ||
         func_name.equals("__local_bh_disable_ip");
}

bool isBhEnableCall(StringRef func_name) {
  return func_name.equals("local_bh_enable") ||
         func_name.equals("__local_bh_enable_ip");
}

bool isPreemptDisableCall(StringRef func_name) {
  return func_name.equals("preempt_disable") ||
         func_name.equals("preempt_disable_notrace");
}

bool isPreemptEnableCall(StringRef func_name) {
  return func_name.equals("preempt_enable") ||
         func_name.equals("preempt_enable_notrace");
}

bool isWorkqueueSubmitCall(StringRef func_name) {
  return func_name.equals("queue_work") || func_name.equals("queue_work_on") ||
         func_name.equals("schedule_work") ||
         func_name.equals("schedule_work_on");
}

bool isWorkqueueInitCall(StringRef func_name) {
  return func_name.equals("INIT_WORK") || func_name.equals("__init_work") ||
         func_name.equals("init_work");
}

const Function *resolveKnownCallee(const CallBase *call) {
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

OperationKind
LinuxKernelProcessModel::classifyOperation(const Instruction *inst,
                                           const StringRef &func_name) const {
  if (LinuxKernelModel::isSpinLock(func_name)) {
    return OperationKind::LOCK_ACQUIRE;
  }
  if (LinuxKernelModel::isSpinUnlock(func_name)) {
    return OperationKind::LOCK_RELEASE;
  }
  if (LinuxKernelModel::isSpinTryLock(func_name)) {
    return OperationKind::LOCK_TRY;
  }
  if (LinuxKernelModel::isSpinLockInit(func_name)) {
    return OperationKind::LOCK_INIT;
  }

  if (LinuxKernelModel::isMutexLock(func_name)) {
    return OperationKind::LOCK_ACQUIRE;
  }
  if (LinuxKernelModel::isMutexTryLock(func_name) ||
      LinuxKernelModel::isMutexConditionalLock(func_name)) {
    return OperationKind::LOCK_TRY;
  }

  if (LinuxKernelModel::isMutexUnlock(func_name)) {
    return OperationKind::LOCK_RELEASE;
  }

  if (LinuxKernelModel::isMutexInit(func_name)) {
    return OperationKind::LOCK_INIT;
  }

  if (LinuxKernelModel::isDown(func_name)) {
    return OperationKind::LOCK_ACQUIRE;
  }
  if (LinuxKernelModel::isDownConditional(func_name) ||
      LinuxKernelModel::isDownTryLock(func_name)) {
    return OperationKind::LOCK_TRY;
  }

  if (LinuxKernelModel::isUp(func_name)) {
    return OperationKind::LOCK_RELEASE;
  }

  if (LinuxKernelModel::isSemaInit(func_name)) {
    return OperationKind::LOCK_INIT;
  }

  if (LinuxKernelModel::isReadLock(func_name) ||
      LinuxKernelModel::isWriteLock(func_name)) {
    return OperationKind::LOCK_ACQUIRE;
  }

  if (LinuxKernelModel::isReadUnlock(func_name) ||
      LinuxKernelModel::isWriteUnlock(func_name)) {
    return OperationKind::LOCK_RELEASE;
  }
  if (LinuxKernelModel::isDownRead(func_name) ||
      LinuxKernelModel::isDownWrite(func_name)) {
    return OperationKind::LOCK_ACQUIRE;
  }
  if (LinuxKernelModel::isDownReadConditional(func_name) ||
      LinuxKernelModel::isDownWriteConditional(func_name) ||
      LinuxKernelModel::isDownReadTryLock(func_name) ||
      LinuxKernelModel::isDownWriteTryLock(func_name)) {
    return OperationKind::LOCK_TRY;
  }
  if (LinuxKernelModel::isUpRead(func_name) ||
      LinuxKernelModel::isUpWrite(func_name)) {
    return OperationKind::LOCK_RELEASE;
  }
  if (LinuxKernelModel::isInitRwsem(func_name)) {
    return OperationKind::LOCK_INIT;
  }

  if (LinuxKernelModel::isRcuReadLock(func_name)) {
    return OperationKind::RCU_READ_LOCK;
  }

  if (LinuxKernelModel::isRcuReadUnlock(func_name)) {
    return OperationKind::RCU_READ_UNLOCK;
  }

  if (LinuxKernelModel::isSynchronizeRcu(func_name)) {
    return OperationKind::RCU_SYNC;
  }

  if (LinuxKernelModel::isCallRcu(func_name)) {
    return OperationKind::RCU_CALL;
  }

  if (LinuxKernelModel::isRcuAssignPointer(func_name)) {
    return OperationKind::RCU_ASSIGN;
  }
  if (LinuxKernelModel::isRcuDereference(func_name)) {
    return OperationKind::RCU_DEREFERENCE;
  }
  if (func_name.equals("kfree") || func_name.equals("kvfree")) {
    return OperationKind::RCU_RECLAIM;
  }

  if (LinuxKernelModel::isWaitForCompletion(func_name)) {
    return OperationKind::COMPLETION_WAIT;
  }

  if (LinuxKernelModel::isComplete(func_name)) {
    return OperationKind::COMPLETION_SIGNAL;
  }

  if (LinuxKernelModel::isInitCompletion(func_name)) {
    return OperationKind::COMPLETION_INIT;
  }
  if (LinuxKernelModel::isReinitCompletion(func_name)) {
    return OperationKind::COMPLETION_REINIT;
  }

  if (LinuxKernelModel::isWaitEvent(func_name)) {
    return OperationKind::WAIT_EVENT;
  }
  if (LinuxKernelModel::isWakeUp(func_name)) {
    return OperationKind::WAKE_UP;
  }
  if (LinuxKernelModel::isInitWaitqueueHead(func_name)) {
    return OperationKind::WAITQUEUE_INIT;
  }
  if (LinuxKernelModel::isPrepareToWait(func_name)) {
    return OperationKind::PREPARE_WAIT;
  }
  if (LinuxKernelModel::isFinishWait(func_name)) {
    return OperationKind::FINISH_WAIT;
  }

  if (LinuxKernelModel::isMemoryBarrier(func_name)) {
    return OperationKind::MEMORY_BARRIER;
  }

  if (isTimerSetupCall(func_name)) {
    return OperationKind::TIMER_SETUP;
  }
  if (isTimerModCall(func_name)) {
    return OperationKind::TIMER_MOD;
  }
  if (isTimerDeleteCall(func_name)) {
    return OperationKind::TIMER_DELETE;
  }
  if (isTimerShutdownCall(func_name)) {
    return OperationKind::TIMER_SHUTDOWN;
  }

  if (LinuxKernelModel::isAtomicRead(func_name)) {
    return OperationKind::ATOMIC_READ;
  }
  if (LinuxKernelModel::isAtomicSet(func_name)) {
    return OperationKind::ATOMIC_WRITE;
  }
  if (LinuxKernelModel::isAtomicAdd(func_name) ||
      LinuxKernelModel::isAtomicSub(func_name) ||
      LinuxKernelModel::isAtomicCmpxchg(func_name) ||
      LinuxKernelModel::isSetBit(func_name)) {
    return OperationKind::ATOMIC_RMW;
  }
  if (LinuxKernelModel::isTestBit(func_name)) {
    return OperationKind::ATOMIC_READ;
  }

  if (LinuxKernelModel::isKthreadCreate(func_name)) {
    return OperationKind::KTHREAD_CREATE;
  }
  if (func_name.equals("kthread_run")) {
    return OperationKind::KTHREAD_RUN;
  }
  if (isWorkqueueInitCall(func_name)) {
    return OperationKind::WORKqueue;
  }
  if (isWorkqueueSubmitCall(func_name)) {
    return OperationKind::WORKqueue_SUBMIT;
  }

  if (isIrqRequestCall(func_name)) {
    return OperationKind::IRQ_REQUEST;
  }
  if (isIrqFreeCall(func_name)) {
    return OperationKind::IRQ_FREE;
  }
  if (isLocalIrqEnableCall(func_name)) {
    return OperationKind::IRQ_ENABLE;
  }
  if (isLocalIrqDisableCall(func_name)) {
    return OperationKind::IRQ_DISABLE;
  }
  if (isLineIrqEnableCall(func_name)) {
    return OperationKind::IRQ_LINE_ENABLE;
  }
  if (isLineIrqDisableCall(func_name)) {
    return OperationKind::IRQ_LINE_DISABLE;
  }
  if (isBhDisableCall(func_name)) {
    return OperationKind::BH_DISABLE;
  }
  if (isBhEnableCall(func_name)) {
    return OperationKind::BH_ENABLE;
  }
  if (isPreemptDisableCall(func_name)) {
    return OperationKind::PREEMPT_DISABLE;
  }
  if (isPreemptEnableCall(func_name)) {
    return OperationKind::PREEMPT_ENABLE;
  }

  return OperationKind::UNKNOWN;
}

LockKind
LinuxKernelProcessModel::classifyLockKind(const StringRef &func_name) const {
  if (LinuxKernelModel::isSpinLock(func_name) ||
      LinuxKernelModel::isSpinUnlock(func_name) ||
      LinuxKernelModel::isSpinLockInit(func_name)) {
    return LockKind::SPINLOCK;
  }
  if (LinuxKernelModel::isMutexLock(func_name) ||
      LinuxKernelModel::isMutexUnlock(func_name) ||
      LinuxKernelModel::isMutexInit(func_name) ||
      LinuxKernelModel::isMutexTryLock(func_name) ||
      LinuxKernelModel::isMutexConditionalLock(func_name)) {
    return LockKind::MUTEX;
  }
  if (LinuxKernelModel::isDown(func_name) ||
      LinuxKernelModel::isDownConditional(func_name) ||
      LinuxKernelModel::isDownTryLock(func_name) ||
      LinuxKernelModel::isUp(func_name) ||
      LinuxKernelModel::isSemaInit(func_name)) {
    return LockKind::SEMAPHORE;
  }
  if (LinuxKernelModel::isReadLock(func_name) ||
      LinuxKernelModel::isWriteLock(func_name)) {
    return LockKind::RWLOCK;
  }
  if (LinuxKernelModel::isDownRead(func_name) ||
      LinuxKernelModel::isUpRead(func_name) ||
      LinuxKernelModel::isDownWrite(func_name) ||
      LinuxKernelModel::isDownReadConditional(func_name) ||
      LinuxKernelModel::isDownWriteConditional(func_name) ||
      LinuxKernelModel::isDownReadTryLock(func_name) ||
      LinuxKernelModel::isDownWriteTryLock(func_name) ||
      LinuxKernelModel::isUpWrite(func_name) ||
      LinuxKernelModel::isInitRwsem(func_name)) {
    return LockKind::RW_SEMAPHORE;
  }
  if (LinuxKernelModel::isRcuReadLock(func_name) ||
      LinuxKernelModel::isRcuReadUnlock(func_name)) {
    return LockKind::RCU;
  }
  if (LinuxKernelModel::isInitCompletion(func_name) ||
      LinuxKernelModel::isComplete(func_name) ||
      LinuxKernelModel::isWaitForCompletion(func_name)) {
    return LockKind::COMPLETION;
  }
  if (LinuxKernelModel::isInitWaitqueueHead(func_name) ||
      LinuxKernelModel::isWakeUp(func_name) ||
      LinuxKernelModel::isWaitEvent(func_name)) {
    return LockKind::WAITQUEUE;
  }

  return LockKind::UNKNOWN;
}

void LinuxKernelProcessModel::extractLockDetails(KernelOperation &op,
                                                 unsigned object_arg_index) {
  const CallBase *cb = dyn_cast<CallBase>(op.inst);
  if (!cb || cb->arg_size() <= object_arg_index) {
    return;
  }

  op.lock = canonicalizeValue(cb->getArgOperand(object_arg_index));

  StringRef func_name(op.function_name);
  op.is_raw = func_name.contains("raw_");
  op.is_nested =
      func_name.contains("nested") || func_name.contains("nest_lock");
  op.is_interruptible =
      func_name.contains("interruptible") || func_name.contains("killable");

  if (func_name.contains("read") && !func_name.contains("write")) {
    op.lock_mode = LockMode::SHARED;
  } else {
    op.lock_mode = LockMode::EXCLUSIVE;
  }

  if (op.kind == OperationKind::LOCK_TRY) {
    if (LinuxKernelModel::isMutexTryLock(func_name) ||
        LinuxKernelModel::isDownReadTryLock(func_name) ||
        LinuxKernelModel::isDownWriteTryLock(func_name) ||
        LinuxKernelModel::isSpinTryLock(func_name)) {
      op.conditional_success = ConditionalSuccess::NONZERO;
    } else {
      // Interruptible/killable locks and down_trylock() acquire on zero.
      op.conditional_success = ConditionalSuccess::ZERO;
    }
  }

  if (func_name.contains("irqsave") || func_name.contains("irqrestore")) {
    op.irq_flags = canonicalizeValue(cb->getArgOperand(cb->arg_size() - 1));
  }
}

void LinuxKernelProcessModel::extractRCUDetails(KernelOperation &op) {
  const CallBase *cb = dyn_cast<CallBase>(op.inst);
  if (!cb) {
    return;
  }

  StringRef func_name(op.function_name);
  if (func_name.contains("srcu")) {
    op.rcu_flavor = RCUFlavor::SRCU;
    if (cb->arg_size() > 0) {
      op.rcu_domain = canonicalizeValue(cb->getArgOperand(0));
    }
  } else if (func_name.contains("tasks_trace") ||
             func_name.contains("read_lock_trace") ||
             func_name.contains("read_unlock_trace")) {
    op.rcu_flavor = RCUFlavor::TASKS_TRACE;
  } else if (func_name.contains("tasks")) {
    op.rcu_flavor = RCUFlavor::TASKS;
  } else if (func_name.contains("_bh")) {
    op.rcu_flavor = RCUFlavor::BH;
  } else if (func_name.contains("sched")) {
    op.rcu_flavor = RCUFlavor::SCHED;
  } else {
    op.rcu_flavor = RCUFlavor::CLASSIC;
  }

  unsigned target_index = 0;
  if (op.rcu_flavor == RCUFlavor::SRCU && op.kind == OperationKind::RCU_CALL) {
    target_index = 1;
  }
  if (cb->arg_size() > target_index &&
      op.kind != OperationKind::RCU_READ_LOCK &&
      op.kind != OperationKind::RCU_READ_UNLOCK &&
      op.kind != OperationKind::RCU_SYNC) {
    op.rcu_target = canonicalizeValue(cb->getArgOperand(target_index));
    op.rcu_sync = op.rcu_target;
  }
  if (op.kind == OperationKind::RCU_CALL && cb->arg_size() > target_index + 1) {
    op.callback = cb->getArgOperand(target_index + 1)->stripPointerCasts();
    op.callbacks.push_back(op.callback);
    op.async_context = AsyncContextKind::RCU_CALLBACK;
  }
  op.requires_rcu_section = !func_name.contains("dereference_protected");
}

void LinuxKernelProcessModel::extractWaitQueueDetails(KernelOperation &op) {
  const CallBase *cb = dyn_cast<CallBase>(op.inst);
  if (!cb || cb->arg_size() == 0) {
    return;
  }

  op.wait_queue = canonicalizeValue(cb->getArgOperand(0));

  if (op.kind == OperationKind::WAIT_EVENT && cb->arg_size() > 1) {
    op.wait_condition = cb->getArgOperand(1);
  }

  StringRef func_name(op.function_name);
  op.is_interruptible =
      func_name.contains("interruptible") || func_name.contains("killable");
  op.has_timeout = func_name.contains("_timeout");
  op.wake_all = func_name.contains("wake_up_all");
  op.wake_exclusive =
      func_name.contains("wake_up_one") || func_name.contains("wake_up_nr");
  if (op.kind == OperationKind::COMPLETION_SIGNAL) {
    op.completion_signal = LinuxKernelModel::isCompleteAll(func_name)
                               ? CompletionSignalKind::ALL
                               : CompletionSignalKind::ONE;
  }
}

void LinuxKernelProcessModel::extractTimerDetails(KernelOperation &op) {
  const CallBase *cb = dyn_cast<CallBase>(op.inst);
  if (!cb || cb->arg_size() == 0) {
    return;
  }

  op.wait_queue = canonicalizeValue(cb->getArgOperand(0));

  if (op.kind == OperationKind::TIMER_MOD && cb->arg_size() > 1) {
    op.timer_expires = cb->getArgOperand(1);
  }
  if (op.kind == OperationKind::TIMER_SETUP && cb->arg_size() > 1) {
    op.callback = cb->getArgOperand(1)->stripPointerCasts();
    op.callbacks.push_back(op.callback);
    op.async_context = AsyncContextKind::TIMER_SOFTIRQ;
  }
}

void LinuxKernelProcessModel::extractAtomicDetails(KernelOperation &op) {
  const CallBase *cb = dyn_cast<CallBase>(op.inst);
  if (!cb || cb->arg_size() == 0) {
    return;
  }

  StringRef func_name(op.function_name);
  unsigned object_index = 0;
  int value_index = -1;

  if (LinuxKernelModel::isSetBit(func_name) ||
      LinuxKernelModel::isTestBit(func_name)) {
    object_index = 1;
    value_index = 0;
  } else if ((LinuxKernelModel::isAtomicAdd(func_name) ||
              LinuxKernelModel::isAtomicSub(func_name)) &&
             !func_name.contains("inc") && !func_name.contains("dec")) {
    object_index = 1;
    value_index = 0;
  } else if (LinuxKernelModel::isAtomicSet(func_name)) {
    value_index = 1;
  } else if (LinuxKernelModel::isAtomicCmpxchg(func_name)) {
    value_index = 1;
  }

  if (object_index >= cb->arg_size()) {
    return;
  }
  op.atomic_var = canonicalizeValue(cb->getArgOperand(object_index));
  if (value_index >= 0 && static_cast<unsigned>(value_index) < cb->arg_size()) {
    if (const auto *const_int =
            dyn_cast<ConstantInt>(cb->getArgOperand(value_index))) {
      if (const_int->getSExtValue() <= std::numeric_limits<int>::max()) {
        op.atomic_value = static_cast<int>(const_int->getSExtValue());
      }
    }
  }
}

void LinuxKernelProcessModel::trackLockState(KernelOperation &op) {
  if ((op.kind == OperationKind::LOCK_ACQUIRE ||
       op.kind == OperationKind::LOCK_TRY) &&
      op.lock != nullptr) {
    auto &lock_info = lock_info_map_[op.lock];
    lock_info.id = op.lock;
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
    lock_info.kind = op.lock_kind;
    lock_info.init_inst = op.inst;
  }
}

void LinuxKernelProcessModel::analyzeLockUsage() {}

void LinuxKernelProcessModel::analyzeModule() {
  all_operations_.clear();
  operation_kind_counts_.clear();
  lock_info_map_.clear();
  rcu_sections_.clear();
  wait_queue_entries_.clear();
  lock_depth_.clear();
  operations_by_function_.clear();
  operation_index_by_inst_.clear();
  instruction_order_.clear();
  canonical_pointer_ids_.clear();

  struct ThinLockWrapper {
    OperationKind kind = OperationKind::UNKNOWN;
    LockKind lock_kind = LockKind::UNKNOWN;
    std::string semantic_name;
    unsigned object_arg_index = 0;
  };
  std::map<const Function *, ThinLockWrapper> thin_lock_wrappers;
  std::map<const Value *, const Value *> registered_work_callbacks;

  for (const Function &function : module_) {
    if (function.isDeclaration()) {
      continue;
    }

    const CallBase *semantic_call = nullptr;
    const Function *semantic_callee = nullptr;
    OperationKind semantic_kind = OperationKind::UNKNOWN;
    bool saw_other_call = false;
    for (const Instruction &instruction : instructions(function)) {
      const auto *call = dyn_cast<CallBase>(&instruction);
      if (call == nullptr) {
        continue;
      }
      const Function *callee = resolveKnownCallee(call);
      if (callee == nullptr) {
        saw_other_call = true;
        break;
      }
      if (callee->isIntrinsic()) {
        continue;
      }
      OperationKind kind = classifyOperation(&instruction, callee->getName());
      if (kind != OperationKind::LOCK_ACQUIRE &&
          kind != OperationKind::LOCK_RELEASE &&
          kind != OperationKind::LOCK_TRY) {
        saw_other_call = true;
        break;
      }
      if (semantic_call != nullptr) {
        saw_other_call = true;
        break;
      }
      semantic_call = call;
      semantic_callee = callee;
      semantic_kind = kind;
    }
    if (saw_other_call || semantic_call == nullptr ||
        semantic_call->arg_size() == 0) {
      continue;
    }

    const Value *inner_object =
        semantic_call->getArgOperand(0)->stripPointerCasts();
    const auto *argument = dyn_cast<Argument>(inner_object);
    if (argument == nullptr || argument->getParent() != &function) {
      continue;
    }

    ThinLockWrapper summary;
    summary.kind = semantic_kind;
    summary.lock_kind = classifyLockKind(semantic_callee->getName());
    summary.semantic_name = semantic_callee->getName().str();
    summary.object_arg_index = argument->getArgNo();
    thin_lock_wrappers[&function] = std::move(summary);
  }

  size_t instruction_order = 0;

  for (Function &F : module_) {
    if (thin_lock_wrappers.count(&F) > 0) {
      continue;
    }
    for (inst_iterator II = inst_begin(F), E = inst_end(F); II != E; ++II) {
      Instruction *I = &*II;
      instruction_order_[I] = instruction_order++;

      const CallBase *cb = dyn_cast<CallBase>(I);
      if (!cb) {
        continue;
      }

      const Function *callee = resolveKnownCallee(cb);
      if (!callee) {
        continue;
      }

      StringRef func_name = callee->getName();
      unsigned lock_object_arg_index = 0;
      OperationKind kind = OperationKind::UNKNOWN;
      LockKind lock_kind = LockKind::UNKNOWN;
      auto wrapper = thin_lock_wrappers.find(callee);
      if (wrapper != thin_lock_wrappers.end()) {
        kind = wrapper->second.kind;
        lock_kind = wrapper->second.lock_kind;
        func_name = wrapper->second.semantic_name;
        lock_object_arg_index = wrapper->second.object_arg_index;
      } else {
        kind = classifyOperation(I, func_name);
        lock_kind = classifyLockKind(func_name);
      }
      if (kind == OperationKind::UNKNOWN) {
        continue;
      }

      KernelOperation op(I, kind, lock_kind);
      op.function_name = func_name.str();

      if (kind == OperationKind::LOCK_ACQUIRE ||
          kind == OperationKind::LOCK_RELEASE ||
          kind == OperationKind::LOCK_TRY || kind == OperationKind::LOCK_INIT) {
        extractLockDetails(op, lock_object_arg_index);
        trackLockState(op);
      } else if (kind == OperationKind::RCU_READ_LOCK ||
                 kind == OperationKind::RCU_READ_UNLOCK ||
                 kind == OperationKind::RCU_SYNC ||
                 kind == OperationKind::RCU_CALL ||
                 kind == OperationKind::RCU_ASSIGN ||
                 kind == OperationKind::RCU_DEREFERENCE ||
                 kind == OperationKind::RCU_RECLAIM) {
        extractRCUDetails(op);
      } else if (kind == OperationKind::WAIT_EVENT ||
                 kind == OperationKind::WAKE_UP ||
                 kind == OperationKind::WAITQUEUE_INIT ||
                 kind == OperationKind::PREPARE_WAIT ||
                 kind == OperationKind::FINISH_WAIT ||
                 kind == OperationKind::COMPLETION_WAIT ||
                 kind == OperationKind::COMPLETION_SIGNAL ||
                 kind == OperationKind::COMPLETION_INIT ||
                 kind == OperationKind::COMPLETION_REINIT) {
        extractWaitQueueDetails(op);
      } else if (kind == OperationKind::TIMER_SETUP ||
                 kind == OperationKind::TIMER_MOD ||
                 kind == OperationKind::TIMER_DELETE ||
                 kind == OperationKind::TIMER_SHUTDOWN) {
        extractTimerDetails(op);
      } else if (kind == OperationKind::ATOMIC_READ ||
                 kind == OperationKind::ATOMIC_WRITE ||
                 kind == OperationKind::ATOMIC_RMW) {
        extractAtomicDetails(op);
      } else if (kind == OperationKind::IRQ_REQUEST) {
        if (cb->arg_size() > 1) {
          op.callback = cb->getArgOperand(1)->stripPointerCasts();
          if (!isa<ConstantPointerNull>(op.callback)) {
            op.callbacks.push_back(op.callback);
            op.async_context = AsyncContextKind::HARDIRQ;
          }
        }
        if (func_name.equals("request_threaded_irq") && cb->arg_size() > 2) {
          const Value *threaded = cb->getArgOperand(2)->stripPointerCasts();
          if (!isa<ConstantPointerNull>(threaded)) {
            op.callbacks.push_back(threaded);
            if (op.async_context == AsyncContextKind::NONE) {
              op.callback = threaded;
              op.async_context = AsyncContextKind::THREADED_IRQ;
            }
          }
        }
      } else if (kind == OperationKind::IRQ_DISABLE ||
                 kind == OperationKind::IRQ_ENABLE) {
        if (func_name.contains("save") || func_name.contains("restore")) {
          if (cb->arg_size() > 0) {
            op.irq_flags =
                canonicalizeValue(cb->getArgOperand(cb->arg_size() - 1));
          } else if (!cb->getType()->isVoidTy()) {
            op.irq_flags = cb;
          }
        }
      } else if (kind == OperationKind::KTHREAD_CREATE ||
                 kind == OperationKind::KTHREAD_RUN) {
        if (cb->arg_size() > 0) {
          op.callback = cb->getArgOperand(0)->stripPointerCasts();
          op.callbacks.push_back(op.callback);
          op.async_context = AsyncContextKind::KTHREAD;
        }
      } else if (kind == OperationKind::WORKqueue) {
        if (cb->arg_size() > 1) {
          op.wait_queue = canonicalizeValue(cb->getArgOperand(0));
          op.callback = cb->getArgOperand(1)->stripPointerCasts();
          op.callbacks.push_back(op.callback);
          op.async_context = AsyncContextKind::WORKQUEUE;
          registered_work_callbacks[op.wait_queue] = op.callback;
        }
      } else if (kind == OperationKind::WORKqueue_SUBMIT) {
        unsigned work_index = func_name.startswith("queue_work") ? 1 : 0;
        if (cb->arg_size() > work_index) {
          op.wait_queue = canonicalizeValue(cb->getArgOperand(work_index));
          auto callback = registered_work_callbacks.find(op.wait_queue);
          if (callback != registered_work_callbacks.end()) {
            op.callback = callback->second;
            op.callbacks.push_back(op.callback);
          }
          op.async_context = AsyncContextKind::WORKQUEUE;
        }
      }

      const size_t index = all_operations_.size();
      all_operations_.push_back(op);
      operation_index_by_inst_[I] = index;
      operations_by_function_[I->getFunction()].push_back(index);
      operation_kind_counts_[kind]++;
    }
  }

  registered_work_callbacks.clear();
  for (const KernelOperation &op : all_operations_) {
    if (op.kind == OperationKind::WORKqueue && op.wait_queue != nullptr &&
        op.callback != nullptr) {
      registered_work_callbacks[op.wait_queue] = op.callback;
    }
  }
  for (KernelOperation &op : all_operations_) {
    if (op.kind != OperationKind::WORKqueue_SUBMIT || op.callback != nullptr) {
      continue;
    }
    auto callback = registered_work_callbacks.find(op.wait_queue);
    if (callback != registered_work_callbacks.end()) {
      op.callback = callback->second;
      op.callbacks.push_back(op.callback);
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
  auto it = operation_index_by_inst_.find(inst);
  if (it == operation_index_by_inst_.end()) {
    return nullptr;
  }

  return &all_operations_[it->second];
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
    StringRef name(op.function_name);
    return name.contains("irqsave") || name.contains("irq_save");
  };
  auto isRestore = [](const KernelOperation &op) {
    StringRef name(op.function_name);
    return name.contains("irqrestore") || name.contains("irq_restore");
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

bool LinuxKernelProcessModel::isInAtomicContext(const Instruction *inst) const {
  if (inst == nullptr) {
    return false;
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
    StringRef name(op->function_name);
    if (name.contains("irqsave") || name.contains("irq_save") ||
        op->kind == OperationKind::IRQ_DISABLE) {
      state.irq_disabled = true;
    } else if (name.contains("irqrestore") || name.contains("irq_restore") ||
               op->kind == OperationKind::IRQ_ENABLE) {
      state.irq_disabled = false;
    }
    if (op->kind == OperationKind::BH_DISABLE) {
      state.bh_disabled = true;
    } else if (op->kind == OperationKind::BH_ENABLE) {
      state.bh_disabled = false;
    }
    if (op->kind == OperationKind::PREEMPT_DISABLE) {
      state.preempt_disabled = true;
    } else if (op->kind == OperationKind::PREEMPT_ENABLE) {
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
      transfer(state, getOperationForInstruction(&instruction));
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
    transfer(state, getOperationForInstruction(&instruction));
  }
  return state.irq_disabled || state.bh_disabled || state.preempt_disabled;
}

bool LinuxKernelProcessModel::maySleep(const Instruction *inst) const {
  const CallBase *cb = dyn_cast<CallBase>(inst);
  if (!cb) {
    return false;
  }

  const Function *callee = resolveKnownCallee(cb);
  if (!callee) {
    return false;
  }

  StringRef func_name = callee->getName();

  return LinuxKernelModel::isMutexLock(func_name) ||
         LinuxKernelModel::isMutexConditionalLock(func_name) ||
         LinuxKernelModel::isDown(func_name) ||
         LinuxKernelModel::isDownConditional(func_name) ||
         LinuxKernelModel::isWaitForCompletion(func_name) ||
         LinuxKernelModel::isWaitEvent(func_name);
}

} // namespace kernel
