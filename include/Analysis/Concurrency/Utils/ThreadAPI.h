/**
 * @file ThreadAPI.h
 * @brief Thread API Recognition and Analysis
 *
 * This file provides utilities for recognizing and analyzing thread-related
 * API calls in multithreaded programs. It supports pthread, OpenMP, and
 * custom threading libraries through a configurable API mapping system.
 *
 * Key Features:
 * - Recognition of thread creation, joining, and termination
 * - Lock acquisition and release operations
 * - Condition variable signaling and waiting
 * - Barrier synchronization support
 * - Configurable API mapping for different threading libraries
 *
 * @author rainoftime
 * @date 2025-2026
 * @ingroup Concurrency
 */

#ifndef THREADAPI_H
#define THREADAPI_H

#include "Analysis/Concurrency/ConcurrencyConfig.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"

#include <string>
#include <unordered_map>

// Do NOT use `using namespace llvm` in headers — it pollutes every TU that
// includes this file.  All LLVM types are qualified explicitly below.

typedef unsigned u32_t;

/**
 * @class ThreadAPI
 * @brief Provides interfaces for recognizing and analyzing pthread/OpenMP
 * programs
 *
 * ThreadAPI is a singleton class that maps function names to thread API types.
 * It enables static analysis of multithreaded programs by identifying:
 * - Thread creation and synchronization points
 * - Lock acquire and release operations
 * - Condition variable operations
 * - Barrier synchronization
 *
 * @note Use ThreadAPI::getThreadAPI() to obtain the singleton instance
 * @note API mappings can be extended via configuration files
 */
class ThreadAPI {

public:
  /**
   * @enum TD_TYPE
   * @brief Thread API function types
   *
   * Enumeration of all supported thread API operation types.
   * Used to classify function calls during static analysis.
   */
  enum TD_TYPE {
    TD_DUMMY = 0,   ///< Unknown or unrecognized API call
    TD_FORK,        ///< Create a new thread (e.g., pthread_create)
    TD_JOIN,        ///< Wait for a thread to join (e.g., pthread_join)
    TD_DETACH,      ///< Detach a thread (e.g., pthread_detach)
    TD_ACQUIRE,     ///< Acquire a lock (e.g., pthread_mutex_lock)
    TD_TRY_ACQUIRE, ///< Try to acquire a lock without blocking (e.g.,
                    ///< pthread_mutex_trylock)
    TD_RWLOCK_RDLOCK, ///< Acquire read lock (e.g., pthread_rwlock_rdlock)
    TD_RWLOCK_WRLOCK, ///< Acquire write lock (e.g., pthread_rwlock_wrlock)
    TD_RELEASE,     ///< Release a lock (e.g., pthread_mutex_unlock)
    TD_EXIT,        ///< Exit/kill a thread (e.g., pthread_exit)
    TD_CANCEL,      ///< Cancel a thread by another (e.g., pthread_cancel)
    TD_COND_WAIT,   ///< Wait on a condition variable (e.g., pthread_cond_wait)
    TD_COND_SIGNAL, ///< Signal a condition variable (e.g., pthread_cond_signal)
    TD_COND_BROADCAST,  ///< Broadcast a condition variable (e.g.,
                        ///< pthread_cond_broadcast)
    TD_MUTEX_INI,       ///< Initialize a mutex
    TD_MUTEX_DESTROY,   ///< Destroy a mutex
    TD_CONDVAR_INI,     ///< Initialize a condition variable
    TD_CONDVAR_DESTROY, ///< Destroy a condition variable
    TD_BAR_INIT,        ///< Initialize a barrier
    TD_BAR_WAIT,        ///< Wait on a barrier
    HARE_PAR_FOR,       ///< Hare parallel for loop construct
    
    // C++11/17/20 Modern Synchronization Primitives
    TD_SHARED_RDLOCK,   ///< std::shared_mutex::lock_shared (read lock)
    TD_SHARED_WRLOCK,   ///< std::shared_mutex::lock (exclusive/write lock)
    TD_SHARED_UNLOCK,   ///< std::shared_mutex::unlock[_shared]
    TD_CALL_ONCE,       ///< std::call_once - singleton initialization
    TD_FUTURE_GET,      ///< std::future::get - synchronization point
    TD_FUTURE_WAIT,     ///< std::future::wait - wait without getting value
    TD_PROMISE_SET,     ///< std::promise::set_value/set_exception
    TD_ASYNC,           ///< std::async - task creation
    
    // RAII Lock Wrappers (special handling needed)
    TD_LOCK_GUARD_CTOR,   ///< std::lock_guard constructor (acquire)
    TD_LOCK_GUARD_DTOR,   ///< std::lock_guard destructor (release)
    TD_UNIQUE_LOCK_CTOR,  ///< std::unique_lock constructor (acquire)
    TD_UNIQUE_LOCK_DTOR,  ///< std::unique_lock destructor (release)
    TD_UNIQUE_LOCK_LOCK,  ///< std::unique_lock::lock (manual acquire)
    TD_UNIQUE_LOCK_UNLOCK, ///< std::unique_lock::unlock (manual release)
    TD_SCOPED_LOCK_CTOR,  ///< std::scoped_lock constructor (acquire multiple)
    TD_SCOPED_LOCK_DTOR,  ///< std::scoped_lock destructor (release multiple)
    TD_SHARED_LOCK_CTOR,  ///< std::shared_lock constructor (shared acquire)
    TD_SHARED_LOCK_DTOR,  ///< std::shared_lock destructor (shared release)
    
    // C++20 Additional Primitives
    TD_JTHREAD_FORK,      ///< std::jthread constructor (fork)
    TD_JTHREAD_JOIN,      ///< std::jthread::join
    TD_LATCH_COUNT_DOWN,  ///< std::latch::count_down
    TD_LATCH_WAIT,        ///< std::latch::wait
    TD_LATCH_ARRIVE_WAIT, ///< std::latch::arrive_and_wait
    TD_BARRIER_ARRIVE_WAIT, ///< std::barrier::arrive_and_wait
    TD_BARRIER_ARRIVE,    ///< std::barrier::arrive
    TD_BARRIER_WAIT_CPP20, ///< std::barrier::wait (C++20 version)
    TD_SEMAPHORE_ACQUIRE, ///< std::counting_semaphore::acquire
    TD_SEMAPHORE_RELEASE, ///< std::counting_semaphore::release
    TD_SEMAPHORE_TRY_ACQUIRE, ///< std::counting_semaphore::try_acquire
    
    // OpenMP Task Support (3.0+)
    TD_OMP_TASK,          ///< __kmpc_omp_task - explicit task creation
    TD_OMP_TASKWAIT,      ///< __kmpc_omp_taskwait - wait for child tasks
    TD_OMP_TASKYIELD,     ///< __kmpc_omp_taskyield - yield to other tasks
    TD_OMP_TASKGROUP_START, ///< __kmpc_taskgroup - start task group
    TD_OMP_TASKGROUP_END, ///< __kmpc_end_taskgroup - end task group
    TD_OMP_TASK_WITH_DEPS, ///< __kmpc_omp_task_with_deps - task with dependencies
    TD_OMP_TASKLOOP,      ///< __kmpc_taskloop - taskloop construct
    
    // OpenMP Additional Constructs
    TD_OMP_SECTIONS_INIT, ///< __kmpc_sections_init - sections construct
    TD_OMP_SECTIONS_NEXT, ///< __kmpc_next_section - get next section
    TD_OMP_SECTIONS_END,  ///< __kmpc_end_sections - end sections
    TD_OMP_ATOMIC_START,  ///< __kmpc_atomic_start - atomic region start
    TD_OMP_ATOMIC_END,    ///< __kmpc_atomic_end - atomic region end
    TD_OMP_FLUSH,         ///< __kmpc_flush - memory fence
    TD_OMP_CANCEL,        ///< __kmpc_cancel - cancellation
    TD_OMP_TARGET,        ///< __tgt_target* - target offloading
    TD_OMP_TARGET_DATA_BEGIN, ///< __tgt_target_data_begin
    TD_OMP_TARGET_DATA_END,   ///< __tgt_target_data_end
    
    // MPI Process Management
    TD_MPI_INIT,           ///< MPI_Init, MPI_Init_thread
    TD_MPI_FINALIZE,       ///< MPI_Finalize
    
    // MPI Point-to-Point (blocking = synchronization point)
    TD_MPI_SEND,           ///< MPI_Send, MPI_Ssend, MPI_Bsend, MPI_Rsend
    TD_MPI_RECV,           ///< MPI_Recv
    TD_MPI_SENDRECV,       ///< MPI_Sendrecv, MPI_Sendrecv_replace
    TD_MPI_PROBE,          ///< MPI_Probe
    
    // MPI Point-to-Point (non-blocking)
    TD_MPI_ISEND,          ///< MPI_Isend, MPI_Issend, MPI_Ibsend, MPI_Irsend
    TD_MPI_IRECV,          ///< MPI_Irecv
    TD_MPI_IPROBE,         ///< MPI_Iprobe
    
    // MPI Synchronization
    TD_MPI_WAIT,           ///< MPI_Wait (join-like for non-blocking ops)
    TD_MPI_WAITALL,        ///< MPI_Waitall
    TD_MPI_WAITANY,        ///< MPI_Waitany
    TD_MPI_WAITSOME,       ///< MPI_Waitsome
    TD_MPI_TEST,           ///< MPI_Test
    TD_MPI_TESTALL,        ///< MPI_Testall
    TD_MPI_TESTANY,        ///< MPI_Testany
    TD_MPI_TESTSOME,       ///< MPI_Testsome
    TD_MPI_BARRIER,        ///< MPI_Barrier, MPI_Ibarrier
    
    // MPI Collectives (all are synchronization points)
    TD_MPI_BCAST,          ///< MPI_Bcast, MPI_Ibcast
    TD_MPI_SCATTER,        ///< MPI_Scatter, MPI_Scatterv, MPI_I*
    TD_MPI_GATHER,         ///< MPI_Gather, MPI_Gatherv, MPI_I*
    TD_MPI_ALLGATHER,      ///< MPI_Allgather, MPI_Allgatherv, MPI_I*
    TD_MPI_ALLTOALL,       ///< MPI_Alltoall, MPI_Alltoallv, MPI_Alltoallw, MPI_I*
    TD_MPI_REDUCE,         ///< MPI_Reduce, MPI_Ireduce
    TD_MPI_ALLREDUCE,      ///< MPI_Allreduce, MPI_Iallreduce
    TD_MPI_REDUCE_SCATTER, ///< MPI_Reduce_scatter, MPI_Reduce_scatter_block, MPI_I*
    TD_MPI_SCAN,           ///< MPI_Scan, MPI_Exscan, MPI_I*
    
    // MPI One-Sided (RMA - Remote Memory Access)
    TD_MPI_WIN_CREATE,     ///< MPI_Win_create, MPI_Win_allocate, MPI_Win_create_dynamic
    TD_MPI_WIN_FREE,       ///< MPI_Win_free
    TD_MPI_PUT,            ///< MPI_Put, MPI_Rput (shared write)
    TD_MPI_GET,            ///< MPI_Get, MPI_Rget (shared read)
    TD_MPI_ACCUMULATE,     ///< MPI_Accumulate, MPI_Get_accumulate, MPI_Fetch_and_op, etc. (atomic RMW)
    
    // MPI RMA Synchronization - Active Target
    TD_MPI_WIN_FENCE,      ///< MPI_Win_fence (barrier for RMA)
    
    // MPI RMA Synchronization - Passive Target
    TD_MPI_WIN_LOCK,       ///< MPI_Win_lock, MPI_Win_lock_all (RMA lock)
    TD_MPI_WIN_UNLOCK,     ///< MPI_Win_unlock, MPI_Win_unlock_all (RMA unlock)
    TD_MPI_WIN_FLUSH,      ///< MPI_Win_flush, MPI_Win_flush_all, MPI_Win_flush_local* (RMA completion)
    TD_MPI_WIN_SYNC,       ///< MPI_Win_sync (memory consistency)
    
    // MPI RMA Synchronization - General Purpose (PSCW)
    TD_MPI_WIN_POST,       ///< MPI_Win_post (exposure epoch start)
    TD_MPI_WIN_START,      ///< MPI_Win_start (access epoch start)
    TD_MPI_WIN_COMPLETE,   ///< MPI_Win_complete (access epoch end)
    TD_MPI_WIN_WAIT,       ///< MPI_Win_wait (exposure epoch end)
    TD_MPI_WIN_TEST,       ///< MPI_Win_test (test exposure epoch)
    
    // MPI Communicator Management
    TD_MPI_COMM_DUP,       ///< MPI_Comm_dup, MPI_Comm_idup
    TD_MPI_COMM_SPLIT,     ///< MPI_Comm_split, MPI_Comm_split_type
    TD_MPI_COMM_CREATE,    ///< MPI_Comm_create, MPI_Comm_create_group
    TD_MPI_COMM_FREE,      ///< MPI_Comm_free
    
    // MPI Request Management
    TD_MPI_REQUEST_FREE,   ///< MPI_Request_free
    TD_MPI_CANCEL,         ///< MPI_Cancel
    
    // Linux Kernel Spinlocks
    TD_KERNEL_SPIN_LOCK_INIT,  ///< spin_lock_init, raw_spin_lock_init
    TD_KERNEL_SPIN_LOCK,       ///< spin_lock, spin_lock_irq, spin_lock_irqsave, spin_lock_bh
    TD_KERNEL_SPIN_UNLOCK,      ///< spin_unlock, spin_unlock_irq, spin_unlock_irqrestore, spin_unlock_bh
    TD_KERNEL_SPIN_TRYLOCK,     ///< spin_trylock, raw_spin_trylock
    
    // Linux Kernel Mutexes
    TD_KERNEL_MUTEX_INIT,       ///< mutex_init, __mutex_init
    TD_KERNEL_MUTEX_LOCK,       ///< mutex_lock, mutex_lock_interruptible, mutex_lock_killable
    TD_KERNEL_MUTEX_UNLOCK,     ///< mutex_unlock
    TD_KERNEL_MUTEX_TRYLOCK,    ///< mutex_trylock
    
    // Linux Kernel Semaphores
    TD_KERNEL_SEMA_INIT,        ///< sema_init, init_MUTEX, init_MUTEX_LOCKED
    TD_KERNEL_DOWN,              ///< down, down_interruptible, down_killable, down_trylock
    TD_KERNEL_UP,                ///< up
    
    // Linux Kernel Read-Write Locks
    TD_KERNEL_READ_LOCK,         ///< read_lock, read_lock_irq, read_lock_irqsave, read_lock_bh
    TD_KERNEL_READ_UNLOCK,       ///< read_unlock, read_unlock_irq, read_unlock_irqrestore, read_unlock_bh
    TD_KERNEL_WRITE_LOCK,        ///< write_lock, write_lock_irq, write_lock_irqsave, write_lock_bh
    TD_KERNEL_WRITE_UNLOCK,      ///< write_unlock, write_unlock_irq, write_unlock_irqrestore, write_unlock_bh
    
    // Linux Kernel Read-Write Semaphores
    TD_KERNEL_DOWN_READ,         ///< down_read, down_read_trylock
    TD_KERNEL_UP_READ,           ///< up_read
    TD_KERNEL_DOWN_WRITE,        ///< down_write, down_write_trylock
    TD_KERNEL_UP_WRITE,          ///< up_write
    TD_KERNEL_INIT_RWSEM,        ///< init_rwsem
    
    // Linux Kernel RCU (Read-Copy-Update)
    TD_KERNEL_RCU_READ_LOCK,     ///< rcu_read_lock, __rcu_read_lock
    TD_KERNEL_RCU_READ_UNLOCK,   ///< rcu_read_unlock, __rcu_read_unlock
    TD_KERNEL_SYNCHRONIZE_RCU,   ///< synchronize_rcu, synchronize_rcu_expedited, synchronize_srcu
    TD_KERNEL_CALL_RCU,          ///< call_rcu, call_srcu
    TD_KERNEL_RCU_DEREFERENCE,   ///< rcu_dereference, rcu_dereference_check, rcu_dereference_protected
    TD_KERNEL_RCU_ASSIGN_POINTER, ///< rcu_assign_pointer
    
    // Linux Kernel Seq Locks
    TD_KERNEL_SEQLOCK_INIT,      ///< seqlock_init
    TD_KERNEL_READ_SEQBEGIN,     ///< read_seqbegin, read_seqbegin_irqsave
    TD_KERNEL_READ_SEQRETRY,     ///< read_seqretry, read_seqretry_irqrestore
    TD_KERNEL_WRITE_SEQLOCK,     ///< write_seqlock, write_seqlock_irq, write_seqlock_irqsave, write_seqlock_bh
    TD_KERNEL_WRITE_SEQUNLOCK,   ///< write_sequnlock, write_sequnlock_irq, write_sequnlock_irqrestore, write_sequnlock_bh
    
    // Linux Kernel Completion Variables
    TD_KERNEL_INIT_COMPLETION,   ///< init_completion
    TD_KERNEL_WAIT_FOR_COMPLETION, ///< wait_for_completion, wait_for_completion_interruptible, wait_for_completion_killable, wait_for_completion_timeout
    TD_KERNEL_COMPLETE,           ///< complete, complete_all
    
    // Linux Kernel Wait Queues
    TD_KERNEL_INIT_WAITQUEUE_HEAD, ///< init_waitqueue_head
    TD_KERNEL_WAIT_EVENT,         ///< wait_event, wait_event_interruptible, wait_event_killable, wait_event_timeout
    TD_KERNEL_WAKE_UP,            ///< wake_up, wake_up_interruptible, wake_up_nr, wake_up_all, wake_up_one
    TD_KERNEL_PREPARE_TO_WAIT,    ///< prepare_to_wait, prepare_to_wait_exclusive
    TD_KERNEL_FINISH_WAIT,        ///< finish_wait
    
    // Linux Kernel Memory Barriers
    TD_KERNEL_MEMORY_BARRIER      ///< mb, rmb, wmb, smp_mb, smp_rmb, smp_wmb, barrier
  };

  /// Map type for API name to TD_TYPE conversion
  using TDAPIMap = llvm::StringMap<TD_TYPE>;

  /// Argument indices for TD_FORK (Goblint-style; default pthread_create: 0,2,3)
  struct ForkArgIndices {
    unsigned thread_arg = 0;
    unsigned start_routine_arg = 2;
    unsigned arg_arg = 3;
  };
  /// Argument indices for TD_JOIN (default pthread_join: 0,1)
  struct JoinArgIndices {
    unsigned thread_arg = 0;
    unsigned ret_arg = 1;
  };
  ForkArgIndices getForkArgIndices(const llvm::Function *F) const;
  JoinArgIndices getJoinArgIndices(const llvm::Function *F) const;

private:
  /// API map, from a string to threadAPI type
  TDAPIMap tdAPIMap;
  std::unordered_map<std::string, ForkArgIndices> m_fork_args;
  std::unordered_map<std::string, JoinArgIndices> m_join_args;
  
  /// Configuration for threading models
  concurrency::ConcurrencyConfig m_config;

  /// Constructor
  ThreadAPI() { init(); }

  /// Initialize the map
  void init();

  /// Static reference
  static ThreadAPI *tdAPI;

  /// Load configuration from a file
  void loadConfig(const std::string &filename);

  /// Add a new entry to the API map
  void addEntry(const std::string &name, TD_TYPE type);

public:
  /// Get the function type if it is a threadAPI function
  TD_TYPE getType(const llvm::Function *F) const;
  
  /// Get the concurrency configuration
  const concurrency::ConcurrencyConfig& getConfig() const { return m_config; }
  
  /// Set the concurrency configuration
  void setConfig(const concurrency::ConcurrencyConfig& config) { m_config = config; }
  
  /// Return a static reference to the singleton instance.
  static ThreadAPI *getThreadAPI() {
    if (tdAPI == nullptr) {
      tdAPI = new ThreadAPI();
    }
    return tdAPI;
  }

  /// Reset the singleton (useful for testing or re-initialization).
  static void resetThreadAPI() {
    delete tdAPI;
    tdAPI = nullptr;
  }

  /// Return the callee/callsite/func
  //@{
  const llvm::Function *getCallee(const llvm::Instruction *inst) const;

  const llvm::Function *getCallee(const llvm::CallBase *cb) const;

  const llvm::CallBase *getLLVMCallSite(const llvm::Instruction *inst) const;
  //@}

  /// Return true if this call create a new thread
  //@{
  inline bool isTDFork(const llvm::Instruction *inst) const {
    return getType(getCallee(inst)) == TD_FORK;
  }
  inline bool isTDFork(const llvm::CallBase *cb) const {
    return getType(getCallee(cb)) == TD_FORK;
  }
  //@}

  /// Return true if this call proceeds a hare_parallel_for
  //@{
  inline bool isHareParFor(const llvm::Instruction *inst) const {
    return getType(getCallee(inst)) == HARE_PAR_FOR;
  }
  inline bool isHareParFor(const llvm::CallBase *cb) const {
    return isHareParFor(llvm::dyn_cast<llvm::Instruction>(cb));
  }
  //@}

  /// Return arguments/attributes of pthread_create / hare_parallel_for
  //@{
  /// Return the thread handle argument (configurable via thread.spec; default 0)
  inline const llvm::Value *getForkedThread(const llvm::Instruction *inst) const {
    assert(isTDFork(inst) && "not a thread fork function!");
    const llvm::CallBase *cb = getLLVMCallSite(inst);
    unsigned idx = getForkArgIndices(getCallee(inst)).thread_arg;
    return cb->getArgOperand(idx);
  }
  inline const llvm::Value *getForkedThread(const llvm::CallBase *cb) const {
    return getForkedThread(llvm::dyn_cast<llvm::Instruction>(cb));
  }

  /// Return the start-routine argument (configurable; default 2)
  inline const llvm::Value *getForkedFun(const llvm::Instruction *inst) const {
    assert(isTDFork(inst) && "not a thread fork function!");
    const llvm::CallBase *cb = getLLVMCallSite(inst);
    unsigned idx = getForkArgIndices(getCallee(inst)).start_routine_arg;
    return cb->getArgOperand(idx)->stripPointerCasts();
  }
  inline const llvm::Value *getForkedFun(const llvm::CallBase *cb) const {
    return getForkedFun(llvm::dyn_cast<llvm::Instruction>(cb));
  }

  /// Return the user-argument passed to the start routine (configurable; default 3)
  inline const llvm::Value *getActualParmAtForkSite(const llvm::Instruction *inst) const {
    assert(isTDFork(inst) && "not a thread fork function!");
    const llvm::CallBase *cb = getLLVMCallSite(inst);
    unsigned idx = getForkArgIndices(getCallee(inst)).arg_arg;
    return cb->getArgOperand(idx);
  }
  inline const llvm::Value *getActualParmAtForkSite(const llvm::CallBase *cb) const {
    return getActualParmAtForkSite(llvm::dyn_cast<llvm::Instruction>(cb));
  }
  //@}

  /// Get the task function (i.e., the 5th parameter) of the hare_parallel_for
  /// call
  //@{
  inline const llvm::Value *
  getTaskFuncAtHareParForSite(const llvm::Instruction *inst) const {
    assert(isHareParFor(inst) && "not a hare_parallel_for function!");
    const llvm::CallBase *cb = getLLVMCallSite(inst);
    return cb->getArgOperand(4)->stripPointerCasts();
  }

  inline const llvm::Value *getTaskFuncAtHareParForSite(const llvm::CallBase *cb) const {
    return getTaskFuncAtHareParForSite(llvm::dyn_cast<llvm::Instruction>(cb));
  }
  //@}

  /// Get the task data (i.e., the 6th parameter) of the hare_parallel_for call
  //@{
  inline const llvm::Value *
  getTaskDataAtHareParForSite(const llvm::Instruction *inst) const {
    assert(isHareParFor(inst) && "not a hare_parallel_for function!");
    const llvm::CallBase *cb = getLLVMCallSite(inst);
    return cb->getArgOperand(5);
  }
  inline const llvm::Value *getTaskDataAtHareParForSite(const llvm::CallBase *cb) const {
    return getTaskDataAtHareParForSite(llvm::dyn_cast<llvm::Instruction>(cb));
  }
  //@}

  /// Return true if this call wait for a worker thread
  //@{
  inline bool isTDJoin(const llvm::Instruction *inst) const {
    return getType(getCallee(inst)) == TD_JOIN;
  }
  inline bool isTDJoin(const llvm::CallBase *cb) const {
    return getType(getCallee(cb)) == TD_JOIN;
  }
  //@}

  /// Return arguments/attributes of pthread_join
  //@{
  /// Return the thread handle argument (configurable via thread.spec; default 0)
  inline const llvm::Value *getJoinedThread(const llvm::Instruction *inst) const {
    assert(isTDJoin(inst) && "not a thread join function!");
    const llvm::CallBase *cb = getLLVMCallSite(inst);
    unsigned idx = getJoinArgIndices(getCallee(inst)).thread_arg;
    llvm::Value *join = cb->getArgOperand(idx);
    if (llvm::isa<llvm::LoadInst>(join))
      return llvm::cast<llvm::LoadInst>(join)->getPointerOperand();
    llvm::Value *stripped = join->stripPointerCasts();
    if (llvm::isa<llvm::Argument>(stripped) || llvm::isa<llvm::AllocaInst>(stripped))
      return stripped;
    if (stripped->getType()->isPointerTy())
      return stripped;
    // Preserve the SSA value for phi/select/scalar forwarding so callers can
    // trace it further instead of giving up immediately.
    return stripped;
  }
  inline const llvm::Value *getJoinedThread(const llvm::CallBase *cb) const {
    return getJoinedThread(llvm::dyn_cast<llvm::Instruction>(cb));
  }
  /// Return the return-value argument (configurable; default 1)
  inline const llvm::Value *getRetParmAtJoinedSite(const llvm::Instruction *inst) const {
    assert(isTDJoin(inst) && "not a thread join function!");
    const llvm::CallBase *cb = getLLVMCallSite(inst);
    unsigned idx = getJoinArgIndices(getCallee(inst)).ret_arg;
    return cb->getArgOperand(idx);
  }
  inline const llvm::Value *getRetParmAtJoinedSite(const llvm::CallBase *cb) const {
    return getRetParmAtJoinedSite(llvm::dyn_cast<llvm::Instruction>(cb));
  }
  //@}

  /// Return true if this call exits/terminate a thread
  //@{
  inline bool isTDExit(const llvm::Instruction *inst) const {
    return getType(getCallee(inst)) == TD_EXIT;
  }

  inline bool isTDExit(const llvm::CallBase *cb) const {
    return getType(getCallee(cb)) == TD_EXIT;
  }
  //@}

  /// Return true if this call acquire a lock (mutex or rwlock read/write)
  //@{
  inline bool isTDAcquire(const llvm::Instruction *inst) const {
    TD_TYPE t = getType(getCallee(inst));
    return t == TD_ACQUIRE || t == TD_TRY_ACQUIRE || t == TD_RWLOCK_RDLOCK ||
           t == TD_RWLOCK_WRLOCK ||
           // Linux kernel locks
           t == TD_KERNEL_SPIN_LOCK || t == TD_KERNEL_SPIN_TRYLOCK ||
           t == TD_KERNEL_MUTEX_LOCK || t == TD_KERNEL_MUTEX_TRYLOCK ||
           t == TD_KERNEL_DOWN || t == TD_KERNEL_READ_LOCK ||
           t == TD_KERNEL_WRITE_LOCK || t == TD_KERNEL_DOWN_READ ||
           t == TD_KERNEL_DOWN_WRITE;
  }

  inline bool isTDAcquire(const llvm::CallBase *cb) const {
    TD_TYPE t = getType(getCallee(cb));
    return t == TD_ACQUIRE || t == TD_TRY_ACQUIRE || t == TD_RWLOCK_RDLOCK ||
           t == TD_RWLOCK_WRLOCK ||
           // Linux kernel locks
           t == TD_KERNEL_SPIN_LOCK || t == TD_KERNEL_SPIN_TRYLOCK ||
           t == TD_KERNEL_MUTEX_LOCK || t == TD_KERNEL_MUTEX_TRYLOCK ||
           t == TD_KERNEL_DOWN || t == TD_KERNEL_READ_LOCK ||
           t == TD_KERNEL_WRITE_LOCK || t == TD_KERNEL_DOWN_READ ||
           t == TD_KERNEL_DOWN_WRITE;
  }
  //@}

  /// Return true if this call release a lock
  //@{
  inline bool isTDRelease(const llvm::Instruction *inst) const {
    TD_TYPE t = getType(getCallee(inst));
    return t == TD_RELEASE ||
           // Linux kernel locks
           t == TD_KERNEL_SPIN_UNLOCK || t == TD_KERNEL_MUTEX_UNLOCK ||
           t == TD_KERNEL_UP || t == TD_KERNEL_READ_UNLOCK ||
           t == TD_KERNEL_WRITE_UNLOCK || t == TD_KERNEL_UP_READ ||
           t == TD_KERNEL_UP_WRITE;
  }

  inline bool isTDRelease(const llvm::CallBase *cb) const {
    TD_TYPE t = getType(getCallee(cb));
    return t == TD_RELEASE ||
           // Linux kernel locks
           t == TD_KERNEL_SPIN_UNLOCK || t == TD_KERNEL_MUTEX_UNLOCK ||
           t == TD_KERNEL_UP || t == TD_KERNEL_READ_UNLOCK ||
           t == TD_KERNEL_WRITE_UNLOCK || t == TD_KERNEL_UP_READ ||
           t == TD_KERNEL_UP_WRITE;
  }
  //@}

  /// Return true if this call is a try-lock (e.g., pthread_mutex_trylock)
  //@{
  inline bool isTryLock(const llvm::Instruction *inst) const {
    TD_TYPE t = getType(getCallee(inst));
    return t == TD_TRY_ACQUIRE ||
           // Linux kernel try-locks
           t == TD_KERNEL_SPIN_TRYLOCK || t == TD_KERNEL_MUTEX_TRYLOCK;
  }
  inline bool isTryLock(const llvm::CallBase *cb) const {
    TD_TYPE t = getType(getCallee(cb));
    return t == TD_TRY_ACQUIRE ||
           // Linux kernel try-locks
           t == TD_KERNEL_SPIN_TRYLOCK || t == TD_KERNEL_MUTEX_TRYLOCK;
  }
  //@}

  /// Return true if this call acquires a read lock (rwlock_rdlock)
  //@{
  inline bool isReadLockAcquire(const llvm::Instruction *inst) const {
    TD_TYPE t = getType(getCallee(inst));
    return t == TD_RWLOCK_RDLOCK ||
           // Linux kernel read locks
           t == TD_KERNEL_READ_LOCK || t == TD_KERNEL_DOWN_READ;
  }
  inline bool isReadLockAcquire(const llvm::CallBase *cb) const {
    TD_TYPE t = getType(getCallee(cb));
    return t == TD_RWLOCK_RDLOCK ||
           // Linux kernel read locks
           t == TD_KERNEL_READ_LOCK || t == TD_KERNEL_DOWN_READ;
  }
  //@}

  /// Return true if this call acquires a write lock (rwlock_wrlock)
  //@{
  inline bool isWriteLockAcquire(const llvm::Instruction *inst) const {
    TD_TYPE t = getType(getCallee(inst));
    return t == TD_RWLOCK_WRLOCK ||
           // Linux kernel write locks
           t == TD_KERNEL_WRITE_LOCK || t == TD_KERNEL_DOWN_WRITE;
  }
  inline bool isWriteLockAcquire(const llvm::CallBase *cb) const {
    TD_TYPE t = getType(getCallee(cb));
    return t == TD_RWLOCK_WRLOCK ||
           // Linux kernel write locks
           t == TD_KERNEL_WRITE_LOCK || t == TD_KERNEL_DOWN_WRITE;
  }
  //@}

  /// Return lock value
  //@{
  /// First argument of pthread_mutex_lock/pthread_mutex_unlock/pthread_rwlock_*
  inline const llvm::Value *getLockVal(const llvm::Instruction *inst) const {
    assert((isTDAcquire(inst) || isTDRelease(inst)) &&
           "not a lock acquire or release function");
    const llvm::CallBase *cb = getLLVMCallSite(inst);
    return cb->getArgOperand(0);
  }
  inline const llvm::Value *getLockVal(const llvm::CallBase *cb) const {
    return getLockVal(llvm::dyn_cast<llvm::Instruction>(cb));
  }
  //@}

  /// Return true if this call waits for a barrier
  //@{
  inline bool isTDBarWait(const llvm::Instruction *inst) const {
    TD_TYPE t = getType(getCallee(inst));
    return t == TD_BAR_WAIT ||
           // Linux kernel barriers (RCU synchronize acts as barrier)
           t == TD_KERNEL_SYNCHRONIZE_RCU;
  }

  inline bool isTDBarWait(const llvm::CallBase *cb) const {
    TD_TYPE t = getType(getCallee(cb));
    return t == TD_BAR_WAIT ||
           // Linux kernel barriers (RCU synchronize acts as barrier)
           t == TD_KERNEL_SYNCHRONIZE_RCU;
  }
  //@}

  /// Return barrier value
  //@{
  /// First argument of pthread_barrier_wait
  inline const llvm::Value *getBarrierVal(const llvm::Instruction *inst) const {
    assert(isTDBarWait(inst) && "not a barrier wait function");
    const llvm::CallBase *cb = getLLVMCallSite(inst);
    return cb->getArgOperand(0);
  }
  inline const llvm::Value *getBarrierVal(const llvm::CallBase *cb) const {
    return getBarrierVal(llvm::dyn_cast<llvm::Instruction>(cb));
  }
  //@}

  /// Return true if this call waits on a condition variable
  //@{
  inline bool isTDCondWait(const llvm::Instruction *inst) const {
    TD_TYPE t = getType(getCallee(inst));
    return t == TD_COND_WAIT ||
           // Linux kernel completion variables and wait queues
           t == TD_KERNEL_WAIT_FOR_COMPLETION || t == TD_KERNEL_WAIT_EVENT;
  }

  inline bool isTDCondWait(const llvm::CallBase *cb) const {
    TD_TYPE t = getType(getCallee(cb));
    return t == TD_COND_WAIT ||
           // Linux kernel completion variables and wait queues
           t == TD_KERNEL_WAIT_FOR_COMPLETION || t == TD_KERNEL_WAIT_EVENT;
  }
  //@}

  /// Return true if this call signals a condition variable
  //@{
  inline bool isTDCondSignal(const llvm::Instruction *inst) const {
    TD_TYPE t = getType(getCallee(inst));
    return t == TD_COND_SIGNAL ||
           // Linux kernel completion variables and wait queues
           t == TD_KERNEL_COMPLETE || t == TD_KERNEL_WAKE_UP;
  }

  inline bool isTDCondSignal(const llvm::CallBase *cb) const {
    TD_TYPE t = getType(getCallee(cb));
    return t == TD_COND_SIGNAL ||
           // Linux kernel completion variables and wait queues
           t == TD_KERNEL_COMPLETE || t == TD_KERNEL_WAKE_UP;
  }
  //@}

  /// Return true if this call broadcasts a condition variable
  //@{
  inline bool isTDCondBroadcast(const llvm::Instruction *inst) const {
    TD_TYPE t = getType(getCallee(inst));
    return t == TD_COND_BROADCAST ||
           // Linux kernel completion variables (complete_all broadcasts)
           t == TD_KERNEL_COMPLETE; // complete_all acts as broadcast
  }

  inline bool isTDCondBroadcast(const llvm::CallBase *cb) const {
    TD_TYPE t = getType(getCallee(cb));
    return t == TD_COND_BROADCAST ||
           // Linux kernel completion variables (complete_all broadcasts)
           t == TD_KERNEL_COMPLETE; // complete_all acts as broadcast
  }
  //@}

  /// Return condition variable value
  //@{
  /// First argument of pthread_cond_wait/signal/broadcast
  inline const llvm::Value *getCondVal(const llvm::Instruction *inst) const {
    assert((isTDCondWait(inst) || isTDCondSignal(inst) ||
            isTDCondBroadcast(inst)) &&
           "not a condition variable function");
    const llvm::CallBase *cb = getLLVMCallSite(inst);
    return cb->getArgOperand(0);
  }
  inline const llvm::Value *getCondVal(const llvm::CallBase *cb) const {
    return getCondVal(llvm::dyn_cast<llvm::Instruction>(cb));
  }
  //@}

  /// Return mutex value associated with condition wait
  //@{
  /// Second argument of pthread_cond_wait
  inline const llvm::Value *getCondMutex(const llvm::Instruction *inst) const {
    assert(isTDCondWait(inst) && "not a condition wait function");
    const llvm::CallBase *cb = getLLVMCallSite(inst);
    return cb->getArgOperand(1);
  }
  inline const llvm::Value *getCondMutex(const llvm::CallBase *cb) const {
    return getCondMutex(llvm::dyn_cast<llvm::Instruction>(cb));
  }
  //@}

  void performAPIStat(llvm::Module *m);
  void statInit(llvm::StringMap<u32_t> &tdAPIStatMap);

  // ========================================================================
  // Convenience group predicates (avoid enumerating all enum values at call sites)
  // ========================================================================

  /// True for any C++20 barrier/latch/semaphore synchronization operation.
  inline bool isBarrierOp(const llvm::Instruction *inst) const {
    TD_TYPE t = getType(getCallee(inst));
    return t == TD_BAR_WAIT || t == TD_BAR_INIT ||
           t == TD_LATCH_COUNT_DOWN || t == TD_LATCH_WAIT ||
           t == TD_LATCH_ARRIVE_WAIT || t == TD_BARRIER_ARRIVE_WAIT ||
           t == TD_BARRIER_ARRIVE || t == TD_BARRIER_WAIT_CPP20;
  }

  /// True for any semaphore acquire/release operation.
  inline bool isSemaphoreOp(const llvm::Instruction *inst) const {
    TD_TYPE t = getType(getCallee(inst));
    return t == TD_SEMAPHORE_ACQUIRE || t == TD_SEMAPHORE_RELEASE ||
           t == TD_SEMAPHORE_TRY_ACQUIRE;
  }

  /// True for any atomic synchronization operation (future/promise/call_once).
  inline bool isAtomicSyncOp(const llvm::Instruction *inst) const {
    TD_TYPE t = getType(getCallee(inst));
    return t == TD_CALL_ONCE || t == TD_FUTURE_GET || t == TD_FUTURE_WAIT ||
           t == TD_PROMISE_SET || t == TD_ASYNC;
  }

  /// True for any MPI collective or barrier (synchronization point).
  inline bool isMPICollective(const llvm::Instruction *inst) const {
    TD_TYPE t = getType(getCallee(inst));
    return t == TD_MPI_BARRIER || t == TD_MPI_BCAST || t == TD_MPI_SCATTER ||
           t == TD_MPI_GATHER || t == TD_MPI_ALLGATHER ||
           t == TD_MPI_ALLTOALL || t == TD_MPI_REDUCE ||
           t == TD_MPI_ALLREDUCE || t == TD_MPI_REDUCE_SCATTER ||
           t == TD_MPI_SCAN;
  }

  /// True for any OpenMP task-related operation.
  inline bool isOMPTaskOp(const llvm::Instruction *inst) const {
    TD_TYPE t = getType(getCallee(inst));
    return t == TD_OMP_TASK || t == TD_OMP_TASKWAIT || t == TD_OMP_TASKYIELD ||
           t == TD_OMP_TASKGROUP_START || t == TD_OMP_TASKGROUP_END ||
           t == TD_OMP_TASK_WITH_DEPS || t == TD_OMP_TASKLOOP;
  }

  /// Convert a TD_TYPE to a human-readable string (for diagnostics).
  static const char *tdTypeToString(TD_TYPE t);

  /// Parse a TD_TYPE name (used by thread.spec and tests).
  static TD_TYPE stringToType(llvm::StringRef name);
};

#endif // THREADAPI_H
