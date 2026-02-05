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

#include "llvm/ADT/StringMap.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"

#include <string>
#include <unordered_map>

using namespace llvm;

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
    HARE_PAR_FOR        ///< Hare parallel for loop construct
  };

  /// Map type for API name to TD_TYPE conversion
  typedef llvm::StringMap<TD_TYPE> TDAPIMap;

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

  /// Constructor
  ThreadAPI() { init(); }

  /// Initialize the map
  void init();

  /// Static reference
  static ThreadAPI *tdAPI;

  /// Get the function type if it is a threadAPI function
  TD_TYPE getType(const Function *F) const;

  /// Load configuration from a file
  void loadConfig(const std::string &filename);

  /// Add a new entry to the API map
  void addEntry(const std::string &name, TD_TYPE type);

public:
  /// Return a static reference
  static ThreadAPI *getThreadAPI() {
    if (tdAPI == NULL) {
      tdAPI = new ThreadAPI();
    }
    return tdAPI;
  }

  /// Return the callee/callsite/func
  //@{
  const llvm::Function *getCallee(const llvm::Instruction *inst) const;

  const llvm::Function *getCallee(const llvm::CallBase *cb) const;

  const llvm::CallBase *getLLVMCallSite(const llvm::Instruction *inst) const;
  //@}

  /// Return true if this call create a new thread
  //@{
  inline bool isTDFork(const Instruction *inst) const {
    return getType(getCallee(inst)) == TD_FORK;
  }
  inline bool isTDFork(const CallBase *cb) const {
    return getType(getCallee(cb)) == TD_FORK;
  }
  //@}

  /// Return true if this call proceeds a hare_parallel_for
  //@{
  inline bool isHareParFor(const Instruction *inst) const {
    return getType(getCallee(inst)) == HARE_PAR_FOR;
  }
  inline bool isHareParFor(const CallBase *cb) const {
    return isHareParFor(dyn_cast<Instruction>(cb));
  }
  //@}

  /// Return arguments/attributes of pthread_create / hare_parallel_for
  //@{
  /// Return the thread handle argument (configurable via thread.spec; default 0)
  inline const Value *getForkedThread(const Instruction *inst) const {
    assert(isTDFork(inst) && "not a thread fork function!");
    const CallBase *cb = getLLVMCallSite(inst);
    unsigned idx = getForkArgIndices(getCallee(inst)).thread_arg;
    return cb->getArgOperand(idx);
  }
  inline const Value *getForkedThread(const CallBase *cb) const {
    return getForkedThread(dyn_cast<Instruction>(cb));
  }

  /// Return the start-routine argument (configurable; default 2)
  inline const Value *getForkedFun(const Instruction *inst) const {
    assert(isTDFork(inst) && "not a thread fork function!");
    const CallBase *cb = getLLVMCallSite(inst);
    unsigned idx = getForkArgIndices(getCallee(inst)).start_routine_arg;
    return cb->getArgOperand(idx)->stripPointerCasts();
  }
  inline const Value *getForkedFun(const CallBase *cb) const {
    return getForkedFun(dyn_cast<Instruction>(cb));
  }

  /// Return the user-argument passed to the start routine (configurable; default 3)
  inline const Value *getActualParmAtForkSite(const Instruction *inst) const {
    assert(isTDFork(inst) && "not a thread fork function!");
    const CallBase *cb = getLLVMCallSite(inst);
    unsigned idx = getForkArgIndices(getCallee(inst)).arg_arg;
    return cb->getArgOperand(idx);
  }
  inline const Value *getActualParmAtForkSite(const CallBase *cb) const {
    return getActualParmAtForkSite(dyn_cast<Instruction>(cb));
  }
  //@}

  /// Get the task function (i.e., the 5th parameter) of the hare_parallel_for
  /// call
  //@{
  inline const Value *
  getTaskFuncAtHareParForSite(const Instruction *inst) const {
    assert(isHareParFor(inst) && "not a hare_parallel_for function!");
    const CallBase *cb = getLLVMCallSite(inst);
    return cb->getArgOperand(4)->stripPointerCasts();
  }

  inline const Value *getTaskFuncAtHareParForSite(const CallBase *cb) const {
    return getTaskFuncAtHareParForSite(dyn_cast<Instruction>(cb));
  }
  //@}

  /// Get the task data (i.e., the 6th parameter) of the hare_parallel_for call
  //@{
  inline const Value *
  getTaskDataAtHareParForSite(const Instruction *inst) const {
    assert(isHareParFor(inst) && "not a hare_parallel_for function!");
    const CallBase *cb = getLLVMCallSite(inst);
    return cb->getArgOperand(5);
  }
  inline const Value *getTaskDataAtHareParForSite(const CallBase *cb) const {
    return getTaskDataAtHareParForSite(dyn_cast<Instruction>(cb));
  }
  //@}

  /// Return true if this call wait for a worker thread
  //@{
  inline bool isTDJoin(const Instruction *inst) const {
    return getType(getCallee(inst)) == TD_JOIN;
  }
  inline bool isTDJoin(const CallBase *cb) const {
    return getType(getCallee(cb)) == TD_JOIN;
  }
  //@}

  /// Return arguments/attributes of pthread_join
  //@{
  /// Return the thread handle argument (configurable via thread.spec; default 0)
  inline const Value *getJoinedThread(const Instruction *inst) const {
    assert(isTDJoin(inst) && "not a thread join function!");
    const CallBase *cb = getLLVMCallSite(inst);
    unsigned idx = getJoinArgIndices(getCallee(inst)).thread_arg;
    Value *join = cb->getArgOperand(idx);
    if (llvm::isa<LoadInst>(join))
      return llvm::cast<LoadInst>(join)->getPointerOperand();
    Value *stripped = join->stripPointerCasts();
    if (llvm::isa<Argument>(stripped) || llvm::isa<AllocaInst>(stripped))
      return stripped;
    if (stripped->getType()->isPointerTy())
      return stripped;
    assert(false && "the value of the first argument at join is unexpected");
    return NULL;
  }
  inline const Value *getJoinedThread(const CallBase *cb) const {
    return getJoinedThread(dyn_cast<Instruction>(cb));
  }
  /// Return the return-value argument (configurable; default 1)
  inline const Value *getRetParmAtJoinedSite(const Instruction *inst) const {
    assert(isTDJoin(inst) && "not a thread join function!");
    const CallBase *cb = getLLVMCallSite(inst);
    unsigned idx = getJoinArgIndices(getCallee(inst)).ret_arg;
    return cb->getArgOperand(idx);
  }
  inline const Value *getRetParmAtJoinedSite(const CallBase *cb) const {
    return getRetParmAtJoinedSite(dyn_cast<Instruction>(cb));
  }
  //@}

  /// Return true if this call exits/terminate a thread
  //@{
  inline bool isTDExit(const Instruction *inst) const {
    return getType(getCallee(inst)) == TD_EXIT;
  }

  inline bool isTDExit(const CallBase *cb) const {
    return getType(getCallee(cb)) == TD_EXIT;
  }
  //@}

  /// Return true if this call acquire a lock (mutex or rwlock read/write)
  //@{
  inline bool isTDAcquire(const Instruction *inst) const {
    TD_TYPE t = getType(getCallee(inst));
    return t == TD_ACQUIRE || t == TD_TRY_ACQUIRE || t == TD_RWLOCK_RDLOCK ||
           t == TD_RWLOCK_WRLOCK;
  }

  inline bool isTDAcquire(const CallBase *cb) const {
    TD_TYPE t = getType(getCallee(cb));
    return t == TD_ACQUIRE || t == TD_TRY_ACQUIRE || t == TD_RWLOCK_RDLOCK ||
           t == TD_RWLOCK_WRLOCK;
  }
  //@}

  /// Return true if this call release a lock
  //@{
  inline bool isTDRelease(const Instruction *inst) const {
    return getType(getCallee(inst)) == TD_RELEASE;
  }

  inline bool isTDRelease(const CallBase *cb) const {
    return getType(getCallee(cb)) == TD_RELEASE;
  }
  //@}

  /// Return true if this call is a try-lock (e.g., pthread_mutex_trylock)
  //@{
  inline bool isTryLock(const Instruction *inst) const {
    return getType(getCallee(inst)) == TD_TRY_ACQUIRE;
  }
  inline bool isTryLock(const CallBase *cb) const {
    return getType(getCallee(cb)) == TD_TRY_ACQUIRE;
  }
  //@}

  /// Return true if this call acquires a read lock (rwlock_rdlock)
  //@{
  inline bool isReadLockAcquire(const Instruction *inst) const {
    return getType(getCallee(inst)) == TD_RWLOCK_RDLOCK;
  }
  inline bool isReadLockAcquire(const CallBase *cb) const {
    return getType(getCallee(cb)) == TD_RWLOCK_RDLOCK;
  }
  //@}

  /// Return true if this call acquires a write lock (rwlock_wrlock)
  //@{
  inline bool isWriteLockAcquire(const Instruction *inst) const {
    return getType(getCallee(inst)) == TD_RWLOCK_WRLOCK;
  }
  inline bool isWriteLockAcquire(const CallBase *cb) const {
    return getType(getCallee(cb)) == TD_RWLOCK_WRLOCK;
  }
  //@}

  /// Return lock value
  //@{
  /// First argument of pthread_mutex_lock/pthread_mutex_unlock/pthread_rwlock_*
  inline const Value *getLockVal(const Instruction *inst) const {
    assert((isTDAcquire(inst) || isTDRelease(inst)) &&
           "not a lock acquire or release function");
    const CallBase *cb = getLLVMCallSite(inst);
    return cb->getArgOperand(0);
  }
  inline const Value *getLockVal(const CallBase *cb) const {
    return getLockVal(dyn_cast<Instruction>(cb));
  }
  //@}

  /// Return true if this call waits for a barrier
  //@{
  inline bool isTDBarWait(const Instruction *inst) const {
    return getType(getCallee(inst)) == TD_BAR_WAIT;
  }

  inline bool isTDBarWait(const CallBase *cb) const {
    return getType(getCallee(cb)) == TD_BAR_WAIT;
  }
  //@}

  /// Return barrier value
  //@{
  /// First argument of pthread_barrier_wait
  inline const Value *getBarrierVal(const Instruction *inst) const {
    assert(isTDBarWait(inst) && "not a barrier wait function");
    const CallBase *cb = getLLVMCallSite(inst);
    return cb->getArgOperand(0);
  }
  inline const Value *getBarrierVal(const CallBase *cb) const {
    return getBarrierVal(dyn_cast<Instruction>(cb));
  }
  //@}

  /// Return true if this call waits on a condition variable
  //@{
  inline bool isTDCondWait(const Instruction *inst) const {
    return getType(getCallee(inst)) == TD_COND_WAIT;
  }

  inline bool isTDCondWait(const CallBase *cb) const {
    return getType(getCallee(cb)) == TD_COND_WAIT;
  }
  //@}

  /// Return true if this call signals a condition variable
  //@{
  inline bool isTDCondSignal(const Instruction *inst) const {
    return getType(getCallee(inst)) == TD_COND_SIGNAL;
  }

  inline bool isTDCondSignal(const CallBase *cb) const {
    return getType(getCallee(cb)) == TD_COND_SIGNAL;
  }
  //@}

  /// Return true if this call broadcasts a condition variable
  //@{
  inline bool isTDCondBroadcast(const Instruction *inst) const {
    return getType(getCallee(inst)) == TD_COND_BROADCAST;
  }

  inline bool isTDCondBroadcast(const CallBase *cb) const {
    return getType(getCallee(cb)) == TD_COND_BROADCAST;
  }
  //@}

  /// Return condition variable value
  //@{
  /// First argument of pthread_cond_wait/signal/broadcast
  inline const Value *getCondVal(const Instruction *inst) const {
    assert((isTDCondWait(inst) || isTDCondSignal(inst) ||
            isTDCondBroadcast(inst)) &&
           "not a condition variable function");
    const CallBase *cb = getLLVMCallSite(inst);
    return cb->getArgOperand(0);
  }
  inline const Value *getCondVal(const CallBase *cb) const {
    return getCondVal(dyn_cast<Instruction>(cb));
  }
  //@}

  /// Return mutex value associated with condition wait
  //@{
  /// Second argument of pthread_cond_wait
  inline const Value *getCondMutex(const Instruction *inst) const {
    assert(isTDCondWait(inst) && "not a condition wait function");
    const CallBase *cb = getLLVMCallSite(inst);
    return cb->getArgOperand(1);
  }
  inline const Value *getCondMutex(const CallBase *cb) const {
    return getCondMutex(dyn_cast<Instruction>(cb));
  }
  //@}

  void performAPIStat(Module *m);
  void statInit(llvm::StringMap<u32_t> &tdAPIStatMap);
};

#endif // THREADAPI_H