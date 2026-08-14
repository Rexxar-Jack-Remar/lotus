/**
 * @file LinuxKernelOperation.h
 * @brief Linux Kernel Operation Types and Structures
 *
 * This file defines the core types, enums, and structures used for
 * Linux Kernel concurrency analysis.
 *
 * @author Lotus Analysis Framework
 * @date 2026
 * @ingroup Concurrency
 */

#ifndef LINUX_KERNEL_OPERATION_H
#define LINUX_KERNEL_OPERATION_H

#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/DenseSet.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>

namespace kernel {

// ============================================================================
// Type Definitions
// ============================================================================

using LockID = const llvm::Value *;
using WaitQueueID = const llvm::Value *;
using RCUSyncPointID = const llvm::Value *;

// ============================================================================
// Enumerations
// ============================================================================

enum class LockKind {
  SPINLOCK,
  MUTEX,
  SEMAPHORE,
  RWLOCK,
  RW_SEMAPHORE,
  COMPLETION,
  WAITQUEUE,
  RCU,
  SEQCOUNT,
  UNKNOWN
};

struct LockClassID {
  const llvm::Value *static_key = nullptr;
  const llvm::Type *aggregate_type = nullptr;
  std::int64_t byte_offset = 0;
  unsigned subclass = 0;
  LockKind kind = LockKind::UNKNOWN;
  bool precise = false;

  bool isValid() const {
    return static_key != nullptr || aggregate_type != nullptr;
  }

  bool operator==(const LockClassID &other) const {
    return std::tie(static_key, aggregate_type, byte_offset, subclass, kind,
                    precise) == std::tie(other.static_key, other.aggregate_type,
                                         other.byte_offset, other.subclass,
                                         other.kind, other.precise);
  }

  bool operator!=(const LockClassID &other) const { return !(*this == other); }

  bool operator<(const LockClassID &other) const {
    return std::tie(static_key, aggregate_type, byte_offset, subclass, kind,
                    precise) < std::tie(other.static_key, other.aggregate_type,
                                        other.byte_offset, other.subclass,
                                        other.kind, other.precise);
  }
};

enum class LockState { UNLOCKED, LOCKED, INTERRUPTIBLE, UNKNOWN };

enum class LockMode { SHARED, EXCLUSIVE, UNKNOWN };

enum class LockReaderKind { NONE, NON_RECURSIVE, RECURSIVE };

enum class ConditionalSuccess {
  UNCONDITIONAL,
  ZERO,
  NONZERO,
};

enum class CompletionSignalKind { ONE, ALL, UNKNOWN };

enum class KernelMemoryOrder {
  NONE,
  RELAXED,
  ACQUIRE,
  RELEASE,
  ACQ_REL,
  FULL,
  COMPILER,
  UNKNOWN,
};

enum class RCUFlavor {
  CLASSIC,
  BH,
  SCHED,
  SRCU,
  TASKS,
  TASKS_TRACE,
  UNKNOWN,
};

enum class AsyncContextKind {
  NONE,
  TASK,
  KTHREAD,
  WORKQUEUE,
  TIMER_SOFTIRQ,
  HARDIRQ,
  THREADED_IRQ,
  RCU_CALLBACK,
  SOFTIRQ,
  TASKLET,
  NAPI,
  NMI,
};

struct AsyncCallbackRegistration {
  const llvm::Value *callback = nullptr;
  AsyncContextKind context = AsyncContextKind::NONE;
  const llvm::Value *object = nullptr;
  const llvm::Value *serialization_domain = nullptr;
  bool serializes_domain = false;
};

enum class OperationKind {
  // Lock operations
  LOCK_ACQUIRE,
  LOCK_RELEASE,
  LOCK_TRY,
  LOCK_INIT,

  // RCU operations
  RCU_READ_LOCK,
  RCU_READ_UNLOCK,
  RCU_SYNC,
  RCU_CALL,
  RCU_ASSIGN,
  RCU_DEREFERENCE,
  RCU_RECLAIM,
  RCU_BARRIER,

  // Sequence lock operations
  SEQLOCK_INIT,
  SEQ_READ_BEGIN,
  SEQ_READ_RETRY,
  SEQ_WRITE_LOCK,
  SEQ_WRITE_UNLOCK,

  // Completion operations
  COMPLETION_WAIT,
  COMPLETION_SIGNAL,
  COMPLETION_INIT,
  COMPLETION_REINIT,

  // Wait queue operations
  WAITQUEUE_INIT,
  WAIT_EVENT,
  WAKE_UP,
  PREPARE_WAIT,
  FINISH_WAIT,

  // Timer operations
  TIMER_SETUP,
  TIMER_MOD,
  TIMER_DELETE,
  TIMER_SHUTDOWN,

  // Memory barriers
  MEMORY_BARRIER,

  // Atomic operations
  ATOMIC_READ,
  ATOMIC_WRITE,
  ATOMIC_RMW,

  // Kthread operations
  KTHREAD_CREATE,
  KTHREAD_START,
  KTHREAD_RUN,
  KTHREAD_STOP,
  KTHREAD_SHOULD_STOP,

  // Workqueue operations
  WORKqueue,
  WORKqueue_CREATE,
  WORKqueue_SUBMIT,
  WORKqueue_FLUSH,
  WORKqueue_CANCEL,
  WORKqueue_DESTROY,

  // Other asynchronous kernel execution mechanisms.
  SOFTIRQ_REGISTER,
  SOFTIRQ_RAISE,
  TASKLET_SETUP,
  TASKLET_SCHEDULE,
  TASKLET_KILL,
  NAPI_REGISTER,
  NAPI_SCHEDULE,
  NAPI_DISABLE,

  // IRQ operations
  IRQ_REQUEST,
  IRQ_FREE,
  IRQ_ENABLE,
  IRQ_DISABLE,
  IRQ_LINE_ENABLE,
  IRQ_LINE_DISABLE,
  BH_ENABLE,
  BH_DISABLE,
  PREEMPT_ENABLE,
  PREEMPT_DISABLE,

  // Memory allocation
  KMALLOC,
  VMALLOC,
  ALLOC_PAGES,
  MEMORY_FREE,

  // Container/List operations
  LIST_ADD,
  LIST_DEL,
  CONTAINER_OF,

  // Calls whose kernel effects could not be summarized.
  UNKNOWN_CALL,

  UNKNOWN
};

// ============================================================================
// Linux Kernel Operation Structure
// ============================================================================

struct KernelOperation {
  const llvm::Instruction *inst;
  OperationKind kind;
  LockKind lock_kind;

  const llvm::Function *function;
  std::string function_name;

  // Lock-related info
  LockID lock = nullptr;
  LockClassID lock_class;
  LockState lock_state = LockState::UNKNOWN;
  LockMode lock_mode = LockMode::UNKNOWN;
  LockReaderKind reader_kind = LockReaderKind::NONE;
  ConditionalSuccess conditional_success = ConditionalSuccess::UNCONDITIONAL;
  bool is_recursive = false;
  bool is_nested = false;
  unsigned lock_subclass = 0;
  bool is_interruptible = false;
  bool is_raw = false;
  bool has_timeout = false;
  bool is_synchronous = false;
  bool serializes_domain = false;
  bool has_unknown_effects = false;
  bool may_sleep = false;
  bool may_spawn = false;
  bool may_access_shared_memory = false;
  bool saves_irq_state = false;
  bool restores_irq_state = false;
  bool disables_local_irq = false;
  bool enables_local_irq = false;
  bool disables_bh = false;
  bool enables_bh = false;
  bool disables_preemption = false;
  bool enables_preemption = false;
  const llvm::Value *irq_flags = nullptr;

  // For wait queues
  WaitQueueID wait_queue = nullptr;
  const llvm::Value *wait_condition = nullptr;
  bool wake_all = false;
  bool wake_exclusive = false;
  CompletionSignalKind completion_signal = CompletionSignalKind::UNKNOWN;

  // For RCU
  RCUSyncPointID rcu_sync = nullptr;
  const llvm::Value *rcu_domain = nullptr;
  const llvm::Value *rcu_target = nullptr;
  const llvm::Value *callback = nullptr;
  std::vector<const llvm::Value *> callbacks;
  std::vector<AsyncCallbackRegistration> async_callbacks;
  AsyncContextKind async_context = AsyncContextKind::NONE;
  const llvm::Value *async_object = nullptr;
  const llvm::Value *serialization_domain = nullptr;
  RCUFlavor rcu_flavor = RCUFlavor::UNKNOWN;
  bool requires_rcu_section = true;
  bool deferred_reclamation = false;
  bool returns_retired_pointer = false;

  // For configured kernel memory operations.
  KernelMemoryOrder memory_order = KernelMemoryOrder::UNKNOWN;

  // For timers
  const llvm::Value *timer_expires = nullptr;

  // For atomic operations
  const llvm::Value *atomic_var = nullptr;
  int atomic_value = 0;

  // Allocation and owner-lifetime information.
  const llvm::Value *memory_object = nullptr;
  const llvm::Value *allocation_size = nullptr;
  bool managed_allocation = false;

  KernelOperation() = default;
  KernelOperation(const llvm::Instruction *i, OperationKind k,
                  LockKind lk = LockKind::UNKNOWN)
      : inst(i), kind(k), lock_kind(lk), function(i->getFunction()) {
    if (function) {
      function_name = function->getName().str();
    }
  }
};

// ============================================================================
// Lock State Tracking
// ============================================================================

struct LockInfo {
  LockID id;
  LockClassID lock_class;
  LockKind kind;
  LockState state = LockState::UNKNOWN;

  const llvm::Instruction *init_inst = nullptr;
  const llvm::Instruction *acquire_inst = nullptr;
  const llvm::Instruction *release_inst = nullptr;

  std::vector<const llvm::Instruction *> acquire_history;
  std::vector<const llvm::Instruction *> release_history;

  bool is_recursive = false;
  bool is_nested = false;
  bool is_interruptible = false;
  bool is_raw = false;

  // For detecting potential deadlocks
  int acquire_count = 0;
  int release_count = 0;
};

// ============================================================================
// RCU Critical Section
// ============================================================================

struct RCUSection {
  const llvm::Instruction *read_lock_inst;
  const llvm::Instruction *read_unlock_inst;
  const llvm::Function *function;

  std::vector<const llvm::Instruction *> protected_reads;

  bool has_sync = false;
  const llvm::Instruction *sync_inst = nullptr;
};

// ============================================================================
// Wait Queue State
// ============================================================================

struct WaitQueueEntry {
  const llvm::Instruction *wait_inst;
  const llvm::Instruction *wake_inst;
  const llvm::Value *queue;

  bool is_interruptible = false;
  bool has_timeout = false;
};

// ============================================================================
// Thread/Process Info
// ============================================================================

struct KernelThreadInfo {
  pid_t tid;
  const llvm::Function *start_function;
  std::vector<const llvm::Instruction *> kernel_operations;

  std::set<LockID> held_locks;
  RCUSection *current_rcu_section = nullptr;
};

} // namespace kernel

#endif // LINUX_KERNEL_OPERATION_H
