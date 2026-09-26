/**
 * @file LinuxKernelOperation.cpp
 * @brief Linux Kernel operation type utilities.
 */

#include "Concurrency/LinuxKernel/LinuxKernelOperation.h"

namespace kernel {

std::string operationKindToString(OperationKind kind) {
  switch (kind) {
  // Lock operations
  case OperationKind::LOCK_ACQUIRE:
    return "LOCK_ACQUIRE";
  case OperationKind::LOCK_RELEASE:
    return "LOCK_RELEASE";
  case OperationKind::LOCK_TRY:
    return "LOCK_TRY";
  case OperationKind::LOCK_INIT:
    return "LOCK_INIT";

  // RCU operations
  case OperationKind::RCU_READ_LOCK:
    return "RCU_READ_LOCK";
  case OperationKind::RCU_READ_UNLOCK:
    return "RCU_READ_UNLOCK";
  case OperationKind::RCU_SYNC:
    return "RCU_SYNC";
  case OperationKind::RCU_CALL:
    return "RCU_CALL";
  case OperationKind::RCU_ASSIGN:
    return "RCU_ASSIGN";
  case OperationKind::RCU_DEREFERENCE:
    return "RCU_DEREFERENCE";
  case OperationKind::RCU_RECLAIM:
    return "RCU_RECLAIM";
  case OperationKind::RCU_BARRIER:
    return "RCU_BARRIER";

  // Sequence lock operations
  case OperationKind::SEQLOCK_INIT:
    return "SEQLOCK_INIT";
  case OperationKind::SEQ_READ_BEGIN:
    return "SEQ_READ_BEGIN";
  case OperationKind::SEQ_READ_RETRY:
    return "SEQ_READ_RETRY";
  case OperationKind::SEQ_WRITE_LOCK:
    return "SEQ_WRITE_LOCK";
  case OperationKind::SEQ_WRITE_UNLOCK:
    return "SEQ_WRITE_UNLOCK";

  // Completion operations
  case OperationKind::COMPLETION_WAIT:
    return "COMPLETION_WAIT";
  case OperationKind::COMPLETION_SIGNAL:
    return "COMPLETION_SIGNAL";
  case OperationKind::COMPLETION_INIT:
    return "COMPLETION_INIT";
  case OperationKind::COMPLETION_REINIT:
    return "COMPLETION_REINIT";

  // Wait queue operations
  case OperationKind::WAITQUEUE_INIT:
    return "WAITQUEUE_INIT";
  case OperationKind::WAIT_EVENT:
    return "WAIT_EVENT";
  case OperationKind::WAKE_UP:
    return "WAKE_UP";
  case OperationKind::PREPARE_WAIT:
    return "PREPARE_WAIT";
  case OperationKind::FINISH_WAIT:
    return "FINISH_WAIT";

  // Timer operations
  case OperationKind::TIMER_SETUP:
    return "TIMER_SETUP";
  case OperationKind::TIMER_MOD:
    return "TIMER_MOD";
  case OperationKind::TIMER_DELETE:
    return "TIMER_DELETE";
  case OperationKind::TIMER_SHUTDOWN:
    return "TIMER_SHUTDOWN";

  // Memory barriers
  case OperationKind::MEMORY_BARRIER:
    return "MEMORY_BARRIER";

  // Atomic operations
  case OperationKind::ATOMIC_READ:
    return "ATOMIC_READ";
  case OperationKind::ATOMIC_WRITE:
    return "ATOMIC_WRITE";
  case OperationKind::ATOMIC_RMW:
    return "ATOMIC_RMW";

  // Kthread operations
  case OperationKind::KTHREAD_CREATE:
    return "KTHREAD_CREATE";
  case OperationKind::KTHREAD_START:
    return "KTHREAD_START";
  case OperationKind::KTHREAD_RUN:
    return "KTHREAD_RUN";
  case OperationKind::KTHREAD_STOP:
    return "KTHREAD_STOP";
  case OperationKind::KTHREAD_SHOULD_STOP:
    return "KTHREAD_SHOULD_STOP";

  // Workqueue operations
  case OperationKind::WORKqueue:
    return "WORKqueue";
  case OperationKind::WORKqueue_CREATE:
    return "WORKqueue_CREATE";
  case OperationKind::WORKqueue_SUBMIT:
    return "WORKqueue_SUBMIT";
  case OperationKind::WORKqueue_FLUSH:
    return "WORKqueue_FLUSH";
  case OperationKind::WORKqueue_CANCEL:
    return "WORKqueue_CANCEL";
  case OperationKind::WORKqueue_DESTROY:
    return "WORKqueue_DESTROY";

  // Other asynchronous kernel execution mechanisms
  case OperationKind::SOFTIRQ_REGISTER:
    return "SOFTIRQ_REGISTER";
  case OperationKind::SOFTIRQ_RAISE:
    return "SOFTIRQ_RAISE";
  case OperationKind::TASKLET_SETUP:
    return "TASKLET_SETUP";
  case OperationKind::TASKLET_SCHEDULE:
    return "TASKLET_SCHEDULE";
  case OperationKind::TASKLET_KILL:
    return "TASKLET_KILL";
  case OperationKind::NAPI_REGISTER:
    return "NAPI_REGISTER";
  case OperationKind::NAPI_SCHEDULE:
    return "NAPI_SCHEDULE";
  case OperationKind::NAPI_DISABLE:
    return "NAPI_DISABLE";

  // IRQ operations
  case OperationKind::IRQ_REQUEST:
    return "IRQ_REQUEST";
  case OperationKind::IRQ_FREE:
    return "IRQ_FREE";
  case OperationKind::IRQ_ENABLE:
    return "IRQ_ENABLE";
  case OperationKind::IRQ_DISABLE:
    return "IRQ_DISABLE";
  case OperationKind::IRQ_LINE_ENABLE:
    return "IRQ_LINE_ENABLE";
  case OperationKind::IRQ_LINE_DISABLE:
    return "IRQ_LINE_DISABLE";
  case OperationKind::BH_ENABLE:
    return "BH_ENABLE";
  case OperationKind::BH_DISABLE:
    return "BH_DISABLE";
  case OperationKind::PREEMPT_ENABLE:
    return "PREEMPT_ENABLE";
  case OperationKind::PREEMPT_DISABLE:
    return "PREEMPT_DISABLE";

  // Memory allocation
  case OperationKind::KMALLOC:
    return "KMALLOC";
  case OperationKind::VMALLOC:
    return "VMALLOC";
  case OperationKind::ALLOC_PAGES:
    return "ALLOC_PAGES";
  case OperationKind::MEMORY_FREE:
    return "MEMORY_FREE";

  // Container/List operations
  case OperationKind::LIST_ADD:
    return "LIST_ADD";
  case OperationKind::LIST_DEL:
    return "LIST_DEL";
  case OperationKind::CONTAINER_OF:
    return "CONTAINER_OF";

  // Calls whose kernel effects could not be summarized
  case OperationKind::UNKNOWN_CALL:
    return "UNKNOWN_CALL";

  case OperationKind::UNKNOWN:
    return "UNKNOWN";
  }
  return "UNKNOWN";
}

} // namespace kernel