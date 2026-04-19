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
         func_name.equals("init_timer_key") || func_name.equals("init_timer");
}

bool isTimerModCall(StringRef func_name) {
  return func_name.equals("mod_timer") || func_name.equals("add_timer") ||
         func_name.equals("add_timer_on");
}

bool isTimerDeleteCall(StringRef func_name) {
  return func_name.equals("del_timer") || func_name.equals("del_timer_sync") ||
         func_name.equals("timer_delete_sync");
}

bool isIrqDisableCall(StringRef func_name) {
  return func_name.equals("local_irq_disable") ||
         func_name.equals("disable_irq") ||
         func_name.equals("disable_irq_nosync");
}

bool isIrqEnableCall(StringRef func_name) {
  return func_name.equals("local_irq_enable") || func_name.equals("enable_irq");
}

bool isIrqRequestCall(StringRef func_name) {
  return func_name.equals("request_irq") || func_name.equals("request_threaded_irq");
}

bool isIrqFreeCall(StringRef func_name) {
  return func_name.equals("free_irq");
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
  if (LinuxKernelModel::isMutexTryLock(func_name)) {
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
    return func_name.endswith("trylock") ? OperationKind::LOCK_TRY
                                         : OperationKind::LOCK_ACQUIRE;
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

  if (LinuxKernelModel::isWaitForCompletion(func_name)) {
    return OperationKind::COMPLETION_WAIT;
  }

  if (LinuxKernelModel::isComplete(func_name)) {
    return OperationKind::COMPLETION_SIGNAL;
  }
  
  if (LinuxKernelModel::isInitCompletion(func_name)) {
    return OperationKind::COMPLETION_INIT;
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

  if (isIrqRequestCall(func_name)) {
    return OperationKind::IRQ_REQUEST;
  }
  if (isIrqFreeCall(func_name)) {
    return OperationKind::IRQ_FREE;
  }
  if (isIrqEnableCall(func_name)) {
    return OperationKind::IRQ_ENABLE;
  }
  if (isIrqDisableCall(func_name)) {
    return OperationKind::IRQ_DISABLE;
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
      LinuxKernelModel::isMutexTryLock(func_name)) {
    return LockKind::MUTEX;
  }
  if (LinuxKernelModel::isDown(func_name) ||
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

void LinuxKernelProcessModel::extractLockDetails(KernelOperation &op) {
  const CallBase *cb = dyn_cast<CallBase>(op.inst);
  if (!cb || cb->arg_size() == 0) {
    return;
  }

  op.lock = canonicalizeValue(cb->getArgOperand(0));

  StringRef func_name(op.function_name);
  op.is_raw = func_name.contains("raw_");
  op.is_interruptible =
      func_name.contains("interruptible") || func_name.contains("killable");
}

void LinuxKernelProcessModel::extractRCUDetails(KernelOperation &op) {
  const CallBase *cb = dyn_cast<CallBase>(op.inst);
  if (!cb) {
    return;
  }

  if (cb->arg_size() > 0) {
    op.rcu_sync = canonicalizeValue(cb->getArgOperand(0));
  } else {
    op.rcu_sync = op.inst;
  }
}

void LinuxKernelProcessModel::extractWaitQueueDetails(KernelOperation &op) {
  const CallBase *cb = dyn_cast<CallBase>(op.inst);
  if (!cb || cb->arg_size() == 0) {
    return;
  }

  op.wait_queue = canonicalizeValue(cb->getArgOperand(0));

  StringRef func_name(op.function_name);
  op.is_interruptible =
      func_name.contains("interruptible") || func_name.contains("killable");
  op.has_timeout = func_name.contains("_timeout");
  op.is_interruptible = op.is_interruptible || op.has_timeout;
}

void LinuxKernelProcessModel::extractTimerDetails(KernelOperation &op) {
  const CallBase *cb = dyn_cast<CallBase>(op.inst);
  if (!cb || cb->arg_size() == 0) {
    return;
  }

  op.wait_queue = canonicalizeValue(cb->getArgOperand(0));

  if (op.kind == OperationKind::TIMER_MOD && cb->arg_size() > 1) {
    const Value *delay = cb->getArgOperand(1);
    if (const auto *const_delay = dyn_cast<ConstantInt>(delay)) {
      if (const_delay->getSExtValue() <= std::numeric_limits<int>::max()) {
        op.timer_delay_ms = static_cast<int>(const_delay->getSExtValue());
      }
    }
  }
}

void LinuxKernelProcessModel::extractAtomicDetails(KernelOperation &op) {
  const CallBase *cb = dyn_cast<CallBase>(op.inst);
  if (!cb || cb->arg_size() == 0) {
    return;
  }

  op.atomic_var = canonicalizeValue(cb->getArgOperand(0));
  if (cb->arg_size() > 1) {
    if (const auto *const_int = dyn_cast<ConstantInt>(cb->getArgOperand(1))) {
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

  size_t instruction_order = 0;

  for (Function &F : module_) {
    for (inst_iterator II = inst_begin(F), E = inst_end(F); II != E; ++II) {
      Instruction *I = &*II;
      instruction_order_[I] = instruction_order++;

      const CallBase *cb = dyn_cast<CallBase>(I);
      if (!cb) {
        continue;
      }

      const Function *callee = cb->getCalledFunction();
      if (!callee) {
        continue;
      }

      StringRef func_name = callee->getName();

      OperationKind kind = classifyOperation(I, func_name);
      if (kind == OperationKind::UNKNOWN) {
        continue;
      }

      LockKind lock_kind = classifyLockKind(func_name);

      KernelOperation op(I, kind, lock_kind);
      op.function_name = func_name.str();

      if (kind == OperationKind::LOCK_ACQUIRE ||
          kind == OperationKind::LOCK_RELEASE ||
          kind == OperationKind::LOCK_TRY ||
          kind == OperationKind::LOCK_INIT) {
        extractLockDetails(op);
        trackLockState(op);
      } else if (kind == OperationKind::RCU_READ_LOCK ||
                 kind == OperationKind::RCU_READ_UNLOCK ||
                 kind == OperationKind::RCU_SYNC ||
                 kind == OperationKind::RCU_CALL ||
                 kind == OperationKind::RCU_ASSIGN ||
                 kind == OperationKind::RCU_DEREFERENCE) {
        extractRCUDetails(op);
      } else if (kind == OperationKind::WAIT_EVENT ||
                 kind == OperationKind::WAKE_UP ||
                 kind == OperationKind::WAITQUEUE_INIT ||
                 kind == OperationKind::PREPARE_WAIT ||
                 kind == OperationKind::FINISH_WAIT ||
                 kind == OperationKind::COMPLETION_WAIT ||
                 kind == OperationKind::COMPLETION_SIGNAL ||
                 kind == OperationKind::COMPLETION_INIT) {
        extractWaitQueueDetails(op);
      } else if (kind == OperationKind::TIMER_SETUP ||
                 kind == OperationKind::TIMER_MOD ||
                 kind == OperationKind::TIMER_DELETE) {
        extractTimerDetails(op);
      } else if (kind == OperationKind::ATOMIC_READ ||
                 kind == OperationKind::ATOMIC_WRITE ||
                 kind == OperationKind::ATOMIC_RMW) {
        extractAtomicDetails(op);
      }

      const size_t index = all_operations_.size();
      all_operations_.push_back(op);
      operation_index_by_inst_[I] = index;
      operations_by_function_[I->getFunction()].push_back(index);
      operation_kind_counts_[kind]++;
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
LinuxKernelProcessModel::getOperationsInFunction(const Function *function) const {
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

const KernelOperation *
LinuxKernelProcessModel::getOperationForInstruction(const Instruction *inst) const {
  auto it = operation_index_by_inst_.find(inst);
  if (it == operation_index_by_inst_.end()) {
    return nullptr;
  }

  return &all_operations_[it->second];
}

bool LinuxKernelProcessModel::isBeforeInFunction(const Instruction *lhs,
                                                 const Instruction *rhs) const {
  if (lhs == nullptr || rhs == nullptr || lhs->getFunction() != rhs->getFunction()) {
    return false;
  }

  auto lhs_it = instruction_order_.find(lhs);
  auto rhs_it = instruction_order_.find(rhs);
  if (lhs_it == instruction_order_.end() || rhs_it == instruction_order_.end()) {
    return false;
  }

  return lhs_it->second < rhs_it->second;
}

const Value *LinuxKernelProcessModel::canonicalizeValue(const Value *value) const {
  if (value == nullptr) {
    return nullptr;
  }

  const Value *stripped = value->stripPointerCasts();
  if (stripped->getType()->isPointerTy()) {
    return getUnderlyingObject(stripped);
  }
  return stripped;
}

std::vector<KernelOperation>
LinuxKernelProcessModel::findLockAcquiresWithoutRelease() const {
  std::vector<KernelOperation> result;

  for (const auto &pair : lock_info_map_) {
    const LockInfo &info = pair.second;
    if (info.acquire_count > info.release_count) {
      if (info.acquire_inst) {
        result.push_back(
            KernelOperation(info.acquire_inst, OperationKind::LOCK_ACQUIRE));
      }
    }
  }

  return result;
}

std::vector<std::pair<const Instruction *, const Instruction *>>
LinuxKernelProcessModel::findPotentialDeadlocks() const {
  std::vector<std::pair<const Instruction *, const Instruction *>> deadlocks;
  return deadlocks;
}

std::vector<const Instruction *>
LinuxKernelProcessModel::findDoubleLocks() const {
  std::vector<const Instruction *> result;
  std::unordered_map<const Function *, std::map<LockID, int>> held_locks;

  for (const auto &op : all_operations_) {
    if (op.lock == nullptr) {
      continue;
    }

    int &depth = held_locks[op.inst->getFunction()][op.lock];
    if ((op.kind == OperationKind::LOCK_ACQUIRE ||
         op.kind == OperationKind::LOCK_TRY) &&
        depth > 0) {
      result.push_back(op.inst);
    }
    if (op.kind == OperationKind::LOCK_ACQUIRE ||
        op.kind == OperationKind::LOCK_TRY) {
      ++depth;
    } else if (op.kind == OperationKind::LOCK_RELEASE && depth > 0) {
      --depth;
    }
  }

  return result;
}

std::vector<const Instruction *>
LinuxKernelProcessModel::findUnlockWithoutLock() const {
  std::vector<const Instruction *> result;

  for (const auto &pair : lock_info_map_) {
    const LockInfo &info = pair.second;
    if (info.release_count > info.acquire_count) {
      if (info.release_inst) {
        result.push_back(info.release_inst);
      }
    }
  }

  return result;
}

std::vector<const Instruction *>
LinuxKernelProcessModel::findMixRawAndcooked() const {
  std::vector<const Instruction *> result;
  std::map<LockID, bool> saw_raw;
  std::map<LockID, bool> saw_cooked;

  for (const auto &op : all_operations_) {
    if (op.lock == nullptr ||
        (op.kind != OperationKind::LOCK_ACQUIRE &&
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
  std::vector<const Instruction *> result;
  std::vector<const Instruction *> syncs;
  for (const auto &op : all_operations_) {
    if (op.kind == OperationKind::RCU_SYNC) {
      syncs.push_back(op.inst);
    }
  }

  for (const auto &op : all_operations_) {
    if (op.kind != OperationKind::RCU_READ_LOCK) {
      continue;
    }

    bool has_sync = llvm::any_of(syncs, [&](const Instruction *sync) {
      return sync->getFunction() == op.inst->getFunction() &&
             isBeforeInFunction(op.inst, sync);
    });
    if (!has_sync) {
      result.push_back(op.inst);
    }
  }
  return result;
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
  std::map<const Value *, const Instruction *> last_delete;

  for (const auto &op : all_operations_) {
    if (op.wait_queue == nullptr) {
      continue;
    }

    if (op.kind == OperationKind::TIMER_DELETE) {
      last_delete[op.wait_queue] = op.inst;
      continue;
    }

    if ((op.kind == OperationKind::TIMER_SETUP ||
         op.kind == OperationKind::TIMER_MOD) &&
        last_delete.count(op.wait_queue) > 0 &&
        isBeforeInFunction(last_delete[op.wait_queue], op.inst)) {
      result.push_back(op.inst);
    }
  }
  return result;
}

std::vector<const Instruction *>
LinuxKernelProcessModel::findIrqSaveRestoreMismatch() const {
  std::vector<const Instruction *> result;
  std::unordered_map<const Function *, std::vector<const Instruction *>> save_stack;

  for (const auto &op : all_operations_) {
    StringRef name(op.function_name);
    auto &stack = save_stack[op.inst->getFunction()];
    if (name.contains("irqsave")) {
      stack.push_back(op.inst);
    } else if (name.contains("irqrestore")) {
      if (stack.empty()) {
        result.push_back(op.inst);
      } else {
        stack.pop_back();
      }
    }
  }

  for (const auto &pair : save_stack) {
    for (const Instruction *save : pair.second) {
      result.push_back(save);
    }
  }
  return result;
}

bool LinuxKernelProcessModel::isInAtomicContext(const Instruction *inst) const {
  if (inst == nullptr) {
    return false;
  }

  std::map<LockID, int> held_locks;
  int irq_disable_depth = 0;

  for (const KernelOperation *op : getOperationsInFunction(inst->getFunction())) {
    StringRef name(op->function_name);
    if (op->inst == inst) {
      break;
    }

    if (name.contains("irqsave") || op->kind == OperationKind::IRQ_DISABLE) {
      ++irq_disable_depth;
    } else if (name.contains("irqrestore") || op->kind == OperationKind::IRQ_ENABLE) {
      irq_disable_depth = std::max(irq_disable_depth - 1, 0);
    }

    if (op->lock == nullptr) {
      continue;
    }
    if (op->kind == OperationKind::LOCK_ACQUIRE ||
        op->kind == OperationKind::LOCK_TRY) {
      held_locks[op->lock]++;
    } else if (op->kind == OperationKind::LOCK_RELEASE &&
               held_locks[op->lock] > 0) {
      held_locks[op->lock]--;
    }
  }

  if (irq_disable_depth > 0) {
    return true;
  }

  for (const auto &pair : held_locks) {
    if (pair.second <= 0) {
      continue;
    }
    auto info_it = lock_info_map_.find(pair.first);
    if (info_it == lock_info_map_.end()) {
      continue;
    }
    if (info_it->second.kind == LockKind::SPINLOCK ||
        info_it->second.kind == LockKind::RWLOCK) {
      return true;
    }
  }

  return false;
}

bool LinuxKernelProcessModel::maySleep(const Instruction *inst) const {
  const CallBase *cb = dyn_cast<CallBase>(inst);
  if (!cb) {
    return false;
  }

  const Function *callee = cb->getCalledFunction();
  if (!callee) {
    return false;
  }

  StringRef func_name = callee->getName();

  return LinuxKernelModel::isMutexLock(func_name) ||
         LinuxKernelModel::isDown(func_name) ||
         LinuxKernelModel::isWaitForCompletion(func_name) ||
         LinuxKernelModel::isWaitEvent(func_name);
}

} // namespace kernel
