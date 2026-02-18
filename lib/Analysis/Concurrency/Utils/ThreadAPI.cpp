/**
 * @file ThreadAPI.cpp
 * @brief Thread API Recognition Implementation
 *
 * This file implements the ThreadAPI singleton class for recognizing and
 * categorizing thread-related API calls in multithreaded programs.
 *
 * @author Lotus Analysis Framework
 * @date 2026 
 * @ingroup Concurrency
 */

/*
 *
 * Author: rainoftime
 */
#include "Analysis/Concurrency/Utils/ThreadAPI.h"

#include "Analysis/Concurrency/Utils/LanguageModel/CppThreading.h"
#include "Analysis/Concurrency/Utils/LanguageModel/LinuxKernel.h"
#include "Analysis/Concurrency/Utils/LanguageModel/MPI.h"
#include "Analysis/Concurrency/Utils/LanguageModel/OpenMP.h"

#include <fstream>
#include <iomanip>
#include <iostream>
#include <set>
#include <sstream>

#include <llvm/ADT/StringMap.h> // for StringMap
#include <llvm/IR/Function.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <stdio.h>

using namespace std;
using namespace llvm;

ThreadAPI *ThreadAPI::tdAPI = nullptr;

/**
 * @struct ei_pair
 * @brief Maps thread API function names to their types
 *
 * Internal structure mapping function name strings to their corresponding
 * TD_TYPE enumeration values for classification.
 */
struct ei_pair {
  const char *n;        ///< Function name
  ThreadAPI::TD_TYPE t; ///< Thread API type
};

/**
 * @brief Thread API mapping table
 *
 * Maps common thread library function names to their semantic types.
 * This enables static recognition of thread operations in LLVM IR.
 */
static const ei_pair ei_pairs[] = {
    // The current llvm-gcc puts in the \01.
    {"pthread_create", ThreadAPI::TD_FORK},
    {"apr_thread_create", ThreadAPI::TD_FORK},
    {"pthread_join", ThreadAPI::TD_JOIN},
    {"\01_pthread_join", ThreadAPI::TD_JOIN},
    {"pthread_cancel", ThreadAPI::TD_JOIN},
    {"pthread_mutex_lock", ThreadAPI::TD_ACQUIRE},
    {"sem_wait", ThreadAPI::TD_ACQUIRE},
    {"_spin_lock", ThreadAPI::TD_ACQUIRE},
    {"SRE_SplSpecLockEx", ThreadAPI::TD_ACQUIRE},
    {"pthread_rwlock_rdlock", ThreadAPI::TD_RWLOCK_RDLOCK},
    {"pthread_rwlock_wrlock", ThreadAPI::TD_RWLOCK_WRLOCK},
    {"pthread_mutex_trylock", ThreadAPI::TD_TRY_ACQUIRE},
    {"pthread_mutex_unlock", ThreadAPI::TD_RELEASE},
    {"pthread_rwlock_unlock", ThreadAPI::TD_RELEASE},
    {"sem_post", ThreadAPI::TD_RELEASE},
    {"_spin_unlock", ThreadAPI::TD_RELEASE},
    {"SRE_SplSpecUnlockEx", ThreadAPI::TD_RELEASE},
    //    {"pthread_cancel", ThreadAPI::TD_CANCEL},
    {"pthread_exit", ThreadAPI::TD_EXIT},
    {"pthread_detach", ThreadAPI::TD_DETACH},
    {"pthread_cond_wait", ThreadAPI::TD_COND_WAIT},
    {"pthread_cond_signal", ThreadAPI::TD_COND_SIGNAL},
    {"pthread_cond_broadcast", ThreadAPI::TD_COND_BROADCAST},
    {"pthread_cond_init", ThreadAPI::TD_CONDVAR_INI},
    {"pthread_cond_destroy", ThreadAPI::TD_CONDVAR_DESTROY},
    {"pthread_mutex_init", ThreadAPI::TD_MUTEX_INI},
    {"pthread_mutex_destroy", ThreadAPI::TD_MUTEX_DESTROY},
    {"pthread_barrier_init", ThreadAPI::TD_BAR_INIT},
    {"pthread_barrier_wait", ThreadAPI::TD_BAR_WAIT},

    // Hare APIs
    {"hare_parallel_for", ThreadAPI::HARE_PAR_FOR},

    // This must be the last entry.
    {0, ThreadAPI::TD_DUMMY}

};

/**
 * @brief Initialize the thread API map
 *
 * Populates the tdAPIMap with function name to type mappings from ei_pairs.
 * Validates that entries are grouped by type for maintainability.
 */
void ThreadAPI::init() {
  set<TD_TYPE> t_seen;
  TD_TYPE prev_t = TD_DUMMY;
  t_seen.insert(TD_DUMMY);
  for (const ei_pair *p = ei_pairs; p->n; ++p) {
    if (p->t != prev_t) {
      // This will detect if you move an entry to another block
      //   but forget to change the type.
      if (t_seen.count(p->t)) {
        fputs(p->n, stderr);
        putc('\n', stderr);
        assert(!"ei_pairs not grouped by type");
      }
      t_seen.insert(p->t);
      prev_t = p->t;
    }
    if (tdAPIMap.count(p->n)) {
      fputs(p->n, stderr);
      putc('\n', stderr);
      assert(!"duplicate name in ei_pairs");
    }
    tdAPIMap[p->n] = p->t;
  }
  // Load optional thread.spec so custom APIs (e.g. kernel mutex_lock) are recognized
  loadConfig("config/thread.spec");
  loadConfig("../config/thread.spec");
}

void ThreadAPI::addEntry(const std::string &name, TD_TYPE type) {
  tdAPIMap[name] = type;
}

static ThreadAPI::TD_TYPE stringToType(const std::string &s) {
  if (s == "TD_FORK")
    return ThreadAPI::TD_FORK;
  if (s == "TD_JOIN")
    return ThreadAPI::TD_JOIN;
  if (s == "TD_DETACH")
    return ThreadAPI::TD_DETACH;
  if (s == "TD_ACQUIRE")
    return ThreadAPI::TD_ACQUIRE;
  if (s == "TD_TRY_ACQUIRE")
    return ThreadAPI::TD_TRY_ACQUIRE;
  if (s == "TD_RWLOCK_RDLOCK")
    return ThreadAPI::TD_RWLOCK_RDLOCK;
  if (s == "TD_RWLOCK_WRLOCK")
    return ThreadAPI::TD_RWLOCK_WRLOCK;
  if (s == "TD_RELEASE")
    return ThreadAPI::TD_RELEASE;
  if (s == "TD_EXIT")
    return ThreadAPI::TD_EXIT;
  if (s == "TD_CANCEL")
    return ThreadAPI::TD_CANCEL;
  if (s == "TD_COND_WAIT")
    return ThreadAPI::TD_COND_WAIT;
  if (s == "TD_COND_SIGNAL")
    return ThreadAPI::TD_COND_SIGNAL;
  if (s == "TD_COND_BROADCAST")
    return ThreadAPI::TD_COND_BROADCAST;
  if (s == "TD_MUTEX_INI")
    return ThreadAPI::TD_MUTEX_INI;
  if (s == "TD_MUTEX_DESTROY")
    return ThreadAPI::TD_MUTEX_DESTROY;
  if (s == "TD_CONDVAR_INI")
    return ThreadAPI::TD_CONDVAR_INI;
  if (s == "TD_CONDVAR_DESTROY")
    return ThreadAPI::TD_CONDVAR_DESTROY;
  if (s == "TD_BAR_INIT")
    return ThreadAPI::TD_BAR_INIT;
  if (s == "TD_BAR_WAIT")
    return ThreadAPI::TD_BAR_WAIT;
  if (s == "HARE_PAR_FOR")
    return ThreadAPI::HARE_PAR_FOR;
  return ThreadAPI::TD_DUMMY;
}

ThreadAPI::ForkArgIndices ThreadAPI::getForkArgIndices(const Function *F) const {
  if (!F) return ForkArgIndices{};
  auto it = m_fork_args.find(F->getName().str());
  if (it != m_fork_args.end()) return it->second;
  return ForkArgIndices{};
}

ThreadAPI::JoinArgIndices ThreadAPI::getJoinArgIndices(const Function *F) const {
  if (!F) return JoinArgIndices{};
  auto it = m_join_args.find(F->getName().str());
  if (it != m_join_args.end()) return it->second;
  return JoinArgIndices{};
}

void ThreadAPI::loadConfig(const std::string &filename) {
  std::ifstream file(filename);
  if (!file.is_open()) {
    return;
  }

  std::string line;
  while (std::getline(file, line)) {
    if (line.empty() || line[0] == '#')
      continue;
    std::stringstream ss(line);
    std::string name, typeStr;
    if (ss >> name >> typeStr) {
      TD_TYPE type = stringToType(typeStr);
      if (type != TD_DUMMY) {
        addEntry(name, type);
        if (type == TD_FORK) {
          unsigned t = 0, s = 2, a = 3;
          if (ss >> t >> s >> a)
            m_fork_args[name] = ForkArgIndices{t, s, a};
        } else if (type == TD_JOIN) {
          unsigned t = 0, r = 1;
          if (ss >> t >> r)
            m_join_args[name] = JoinArgIndices{t, r};
        }
      }
    }
  }
}

ThreadAPI::TD_TYPE ThreadAPI::getType(const Function *F) const {
  if (!F)
    return TD_DUMMY;

  // 1. Exact match (including loaded config)
  std::string nameStr = F->getName().str();
  TDAPIMap::const_iterator it = tdAPIMap.find(nameStr);
  if (it != tdAPIMap.end())
    return it->second;

  // Try with LLVM name prefix stripped (e.g. \01) and leading underscore (macOS)
  if (nameStr.size() > 0 && nameStr[0] == '\01')
    nameStr = nameStr.substr(1);
  if (!nameStr.empty() && nameStr[0] == '_')
    nameStr = nameStr.substr(1);
  it = tdAPIMap.find(nameStr);
  if (it != tdAPIMap.end())
    return it->second;

  StringRef name = F->getName();

  // 2. OpenMP Support (if enabled)
  if (m_config.enable_openmp()) {
    if (OpenMPModel::isFork(name))
      return TD_FORK;
    if (OpenMPModel::isBarrier(name))
      return TD_BAR_WAIT;
    if (OpenMPModel::isSetLock(name) || OpenMPModel::isSetNestLock(name) ||
        OpenMPModel::isCriticalStart(name))
      return TD_ACQUIRE;
    if (OpenMPModel::isUnsetLock(name) || OpenMPModel::isUnsetNestLock(name) ||
        OpenMPModel::isCriticalEnd(name))
      return TD_RELEASE;
    
    // OpenMP Task Support (3.0+)
    if (OpenMPModel::isTask(name))
      return TD_OMP_TASK;
    if (OpenMPModel::isTaskwait(name))
      return TD_OMP_TASKWAIT;
    if (OpenMPModel::isTaskyield(name))
      return TD_OMP_TASKYIELD;
    if (OpenMPModel::isTaskgroupStart(name))
      return TD_OMP_TASKGROUP_START;
    if (OpenMPModel::isTaskgroupEnd(name))
      return TD_OMP_TASKGROUP_END;
    if (OpenMPModel::isTaskWithDeps(name))
      return TD_OMP_TASK_WITH_DEPS;
    if (OpenMPModel::isTaskloop(name) || OpenMPModel::isTaskloopNoWait(name))
      return TD_OMP_TASKLOOP;
    
    // OpenMP Sections
    if (OpenMPModel::isSectionsInit(name))
      return TD_OMP_SECTIONS_INIT;
    if (OpenMPModel::isSectionsNext(name))
      return TD_OMP_SECTIONS_NEXT;
    if (OpenMPModel::isSectionsEnd(name))
      return TD_OMP_SECTIONS_END;
    
    // OpenMP Atomic
    if (OpenMPModel::isAtomicStart(name))
      return TD_OMP_ATOMIC_START;
    if (OpenMPModel::isAtomicEnd(name))
      return TD_OMP_ATOMIC_END;
    
    // OpenMP Flush
    if (OpenMPModel::isFlush(name))
      return TD_OMP_FLUSH;
    
    // OpenMP Cancellation
    if (OpenMPModel::isCancel(name) || OpenMPModel::isCancellationPoint(name))
      return TD_OMP_CANCEL;
    
    // OpenMP Target Offloading
    if (OpenMPModel::isTargetInit(name))
      return TD_OMP_TARGET;
    if (OpenMPModel::isTargetDataBegin(name))
      return TD_OMP_TARGET_DATA_BEGIN;
    if (OpenMPModel::isTargetDataEnd(name))
      return TD_OMP_TARGET_DATA_END;
  }

  // 3. C++11/17/20 Support (if enabled)
  if (m_config.enable_cpp11()) {
    // Basic threading
    if (CppThreadingModel::isFork(name))
      return TD_FORK;
    if (CppThreadingModel::isJoin(name))
      return TD_JOIN;
    if (CppThreadingModel::isDetach(name))
      return TD_DETACH;
    
    // Basic mutex operations
    if (CppThreadingModel::isAcquire(name))
      return TD_ACQUIRE;
    if (CppThreadingModel::isTryAcquire(name))
      return TD_TRY_ACQUIRE;
    if (CppThreadingModel::isRelease(name))
      return TD_RELEASE;
    
    // Condition variables
    if (CppThreadingModel::isCondWait(name))
      return TD_COND_WAIT;
    if (CppThreadingModel::isCondSignal(name))
      return TD_COND_SIGNAL;
    if (CppThreadingModel::isCondBroadcast(name))
      return TD_COND_BROADCAST;
    
    // C++17 shared_mutex
    if (CppThreadingModel::isSharedLockAcquire(name) || CppThreadingModel::isSharedTimedLockAcquire(name))
      return TD_SHARED_RDLOCK;
    if (CppThreadingModel::isSharedLockExclusiveAcquire(name) || CppThreadingModel::isSharedTimedLockExclusiveAcquire(name))
      return TD_SHARED_WRLOCK;
    if (CppThreadingModel::isSharedLockRelease(name) || CppThreadingModel::isSharedLockExclusiveRelease(name))
      return TD_SHARED_UNLOCK;
    
    // RAII lock wrappers
    if (CppThreadingModel::isLockGuardConstructor(name))
      return TD_LOCK_GUARD_CTOR;
    if (CppThreadingModel::isLockGuardDestructor(name))
      return TD_LOCK_GUARD_DTOR;
    if (CppThreadingModel::isUniqueLockConstructor(name))
      return TD_UNIQUE_LOCK_CTOR;
    if (CppThreadingModel::isUniqueLockDestructor(name))
      return TD_UNIQUE_LOCK_DTOR;
    if (CppThreadingModel::isUniqueLockLock(name))
      return TD_UNIQUE_LOCK_LOCK;
    if (CppThreadingModel::isUniqueLockUnlock(name))
      return TD_UNIQUE_LOCK_UNLOCK;
    if (CppThreadingModel::isScopedLockConstructor(name))
      return TD_SCOPED_LOCK_CTOR;
    if (CppThreadingModel::isScopedLockDestructor(name))
      return TD_SCOPED_LOCK_DTOR;
    if (CppThreadingModel::isSharedLockConstructor(name))
      return TD_SHARED_LOCK_CTOR;
    if (CppThreadingModel::isSharedLockDestructor(name))
      return TD_SHARED_LOCK_DTOR;
    
    // std::call_once
    if (CppThreadingModel::isCallOnce(name))
      return TD_CALL_ONCE;
    
    // Future/Promise synchronization
    if (CppThreadingModel::isFutureGet(name))
      return TD_FUTURE_GET;
    if (CppThreadingModel::isFutureWait(name))
      return TD_FUTURE_WAIT;
    if (CppThreadingModel::isPromiseSetValue(name) || CppThreadingModel::isPromiseSetException(name))
      return TD_PROMISE_SET;
    if (CppThreadingModel::isAsync(name))
      return TD_ASYNC;
    
    // C++20 jthread
    if (CppThreadingModel::isJthreadConstructor(name))
      return TD_JTHREAD_FORK;
    if (CppThreadingModel::isJthreadJoin(name))
      return TD_JTHREAD_JOIN;
    
    // C++20 latch
    if (CppThreadingModel::isLatchCountDown(name))
      return TD_LATCH_COUNT_DOWN;
    if (CppThreadingModel::isLatchWait(name))
      return TD_LATCH_WAIT;
    if (CppThreadingModel::isLatchArriveAndWait(name))
      return TD_LATCH_ARRIVE_WAIT;
    
    // C++20 barrier
    if (CppThreadingModel::isBarrierArriveAndWait(name))
      return TD_BARRIER_ARRIVE_WAIT;
    if (CppThreadingModel::isBarrierArrive(name))
      return TD_BARRIER_ARRIVE;
    if (CppThreadingModel::isBarrierWait(name))
      return TD_BARRIER_WAIT_CPP20;
    
    // C++20 semaphore
    if (CppThreadingModel::isSemaphoreAcquire(name))
      return TD_SEMAPHORE_ACQUIRE;
    if (CppThreadingModel::isSemaphoreRelease(name))
      return TD_SEMAPHORE_RELEASE;
    if (CppThreadingModel::isSemaphoreTryAcquire(name))
      return TD_SEMAPHORE_TRY_ACQUIRE;
  }

  // 4. MPI Support (if enabled)
  if (m_config.enable_mpi()) {
    // Process management
    if (MPIModel::isInit(name))
      return TD_MPI_INIT;
    if (MPIModel::isFinalize(name))
      return TD_MPI_FINALIZE;
      
    // Point-to-point blocking
    if (MPIModel::isSend(name))
      return TD_MPI_SEND;
    if (MPIModel::isRecv(name))
      return TD_MPI_RECV;
    if (MPIModel::isSendrecv(name))
      return TD_MPI_SENDRECV;
    if (MPIModel::isProbe(name))
      return TD_MPI_PROBE;
      
    // Point-to-point non-blocking
    if (MPIModel::isIsend(name))
      return TD_MPI_ISEND;
    if (MPIModel::isIrecv(name))
      return TD_MPI_IRECV;
    if (MPIModel::isIprobe(name))
      return TD_MPI_IPROBE;
      
    // Synchronization
    if (MPIModel::isWait(name))
      return TD_MPI_WAIT;
    if (MPIModel::isWaitall(name))
      return TD_MPI_WAITALL;
    if (MPIModel::isWaitany(name))
      return TD_MPI_WAITANY;
    if (MPIModel::isWaitsome(name))
      return TD_MPI_WAITSOME;
    if (MPIModel::isTest(name))
      return TD_MPI_TEST;
    if (MPIModel::isTestall(name))
      return TD_MPI_TESTALL;
    if (MPIModel::isTestany(name))
      return TD_MPI_TESTANY;
    if (MPIModel::isTestsome(name))
      return TD_MPI_TESTSOME;
    if (MPIModel::isBarrier(name))
      return TD_MPI_BARRIER;
      
    // Collectives
    if (MPIModel::isBcast(name))
      return TD_MPI_BCAST;
    if (MPIModel::isScatter(name))
      return TD_MPI_SCATTER;
    if (MPIModel::isGather(name))
      return TD_MPI_GATHER;
    if (MPIModel::isAllgather(name))
      return TD_MPI_ALLGATHER;
    if (MPIModel::isAlltoall(name))
      return TD_MPI_ALLTOALL;
    if (MPIModel::isReduce(name))
      return TD_MPI_REDUCE;
    if (MPIModel::isAllreduce(name))
      return TD_MPI_ALLREDUCE;
    if (MPIModel::isReduceScatter(name))
      return TD_MPI_REDUCE_SCATTER;
    if (MPIModel::isScan(name))
      return TD_MPI_SCAN;
      
    // RMA (one-sided)
    if (MPIModel::isWinCreate(name))
      return TD_MPI_WIN_CREATE;
    if (MPIModel::isWinFree(name))
      return TD_MPI_WIN_FREE;
    if (MPIModel::isPut(name))
      return TD_MPI_PUT;
    if (MPIModel::isGet(name))
      return TD_MPI_GET;
    if (MPIModel::isAccumulate(name))
      return TD_MPI_ACCUMULATE;
    
    // RMA synchronization
    if (MPIModel::isWinFence(name))
      return TD_MPI_WIN_FENCE;
    if (MPIModel::isWinLock(name))
      return TD_MPI_WIN_LOCK;
    if (MPIModel::isWinUnlock(name))
      return TD_MPI_WIN_UNLOCK;
    if (MPIModel::isWinFlush(name))
      return TD_MPI_WIN_FLUSH;
    if (MPIModel::isWinSync(name))
      return TD_MPI_WIN_SYNC;
    if (MPIModel::isWinPost(name))
      return TD_MPI_WIN_POST;
    if (MPIModel::isWinStart(name))
      return TD_MPI_WIN_START;
    if (MPIModel::isWinComplete(name))
      return TD_MPI_WIN_COMPLETE;
    if (MPIModel::isWinWait(name))
      return TD_MPI_WIN_WAIT;
    if (MPIModel::isWinTest(name))
      return TD_MPI_WIN_TEST;
    
    // Communicator management
    if (MPIModel::isCommDup(name))
      return TD_MPI_COMM_DUP;
    if (MPIModel::isCommSplit(name))
      return TD_MPI_COMM_SPLIT;
    if (MPIModel::isCommCreate(name))
      return TD_MPI_COMM_CREATE;
    if (MPIModel::isCommFree(name))
      return TD_MPI_COMM_FREE;
    
    // Request management
    if (MPIModel::isRequestFree(name))
      return TD_MPI_REQUEST_FREE;
    if (MPIModel::isCancel(name))
      return TD_MPI_CANCEL;
  }

  // 5. Linux Kernel Support (if enabled)
  if (m_config.enable_linux_kernel()) {
    // Spinlocks
    if (LinuxKernelModel::isSpinLockInit(name))
      return TD_KERNEL_SPIN_LOCK_INIT;
    if (LinuxKernelModel::isSpinLock(name))
      return TD_KERNEL_SPIN_LOCK;
    if (LinuxKernelModel::isSpinUnlock(name))
      return TD_KERNEL_SPIN_UNLOCK;
    if (LinuxKernelModel::isSpinTryLock(name))
      return TD_KERNEL_SPIN_TRYLOCK;
    
    // Mutexes
    if (LinuxKernelModel::isMutexInit(name))
      return TD_KERNEL_MUTEX_INIT;
    if (LinuxKernelModel::isMutexLock(name))
      return TD_KERNEL_MUTEX_LOCK;
    if (LinuxKernelModel::isMutexUnlock(name))
      return TD_KERNEL_MUTEX_UNLOCK;
    if (LinuxKernelModel::isMutexTryLock(name))
      return TD_KERNEL_MUTEX_TRYLOCK;
    
    // Semaphores
    if (LinuxKernelModel::isSemaInit(name))
      return TD_KERNEL_SEMA_INIT;
    if (LinuxKernelModel::isDown(name))
      return TD_KERNEL_DOWN;
    if (LinuxKernelModel::isUp(name))
      return TD_KERNEL_UP;
    
    // Read-Write Locks
    if (LinuxKernelModel::isReadLock(name))
      return TD_KERNEL_READ_LOCK;
    if (LinuxKernelModel::isReadUnlock(name))
      return TD_KERNEL_READ_UNLOCK;
    if (LinuxKernelModel::isWriteLock(name))
      return TD_KERNEL_WRITE_LOCK;
    if (LinuxKernelModel::isWriteUnlock(name))
      return TD_KERNEL_WRITE_UNLOCK;
    
    // Read-Write Semaphores
    if (LinuxKernelModel::isDownRead(name))
      return TD_KERNEL_DOWN_READ;
    if (LinuxKernelModel::isUpRead(name))
      return TD_KERNEL_UP_READ;
    if (LinuxKernelModel::isDownWrite(name))
      return TD_KERNEL_DOWN_WRITE;
    if (LinuxKernelModel::isUpWrite(name))
      return TD_KERNEL_UP_WRITE;
    if (LinuxKernelModel::isInitRwsem(name))
      return TD_KERNEL_INIT_RWSEM;
    
    // RCU
    if (LinuxKernelModel::isRcuReadLock(name))
      return TD_KERNEL_RCU_READ_LOCK;
    if (LinuxKernelModel::isRcuReadUnlock(name))
      return TD_KERNEL_RCU_READ_UNLOCK;
    if (LinuxKernelModel::isSynchronizeRcu(name))
      return TD_KERNEL_SYNCHRONIZE_RCU;
    if (LinuxKernelModel::isCallRcu(name))
      return TD_KERNEL_CALL_RCU;
    if (LinuxKernelModel::isRcuDereference(name))
      return TD_KERNEL_RCU_DEREFERENCE;
    if (LinuxKernelModel::isRcuAssignPointer(name))
      return TD_KERNEL_RCU_ASSIGN_POINTER;
    
    // Seq Locks
    if (LinuxKernelModel::isSeqlockInit(name))
      return TD_KERNEL_SEQLOCK_INIT;
    if (LinuxKernelModel::isReadSeqbegin(name))
      return TD_KERNEL_READ_SEQBEGIN;
    if (LinuxKernelModel::isReadSeqretry(name))
      return TD_KERNEL_READ_SEQRETRY;
    if (LinuxKernelModel::isWriteSeqlock(name))
      return TD_KERNEL_WRITE_SEQLOCK;
    if (LinuxKernelModel::isWriteSequnlock(name))
      return TD_KERNEL_WRITE_SEQUNLOCK;
    
    // Completion Variables
    if (LinuxKernelModel::isInitCompletion(name))
      return TD_KERNEL_INIT_COMPLETION;
    if (LinuxKernelModel::isWaitForCompletion(name))
      return TD_KERNEL_WAIT_FOR_COMPLETION;
    if (LinuxKernelModel::isComplete(name))
      return TD_KERNEL_COMPLETE;
    
    // Wait Queues
    if (LinuxKernelModel::isInitWaitqueueHead(name))
      return TD_KERNEL_INIT_WAITQUEUE_HEAD;
    if (LinuxKernelModel::isWaitEvent(name))
      return TD_KERNEL_WAIT_EVENT;
    if (LinuxKernelModel::isWakeUp(name))
      return TD_KERNEL_WAKE_UP;
    if (LinuxKernelModel::isPrepareToWait(name))
      return TD_KERNEL_PREPARE_TO_WAIT;
    if (LinuxKernelModel::isFinishWait(name))
      return TD_KERNEL_FINISH_WAIT;
    
    // Memory Barriers
    if (LinuxKernelModel::isMemoryBarrier(name))
      return TD_KERNEL_MEMORY_BARRIER;
  }

  return TD_DUMMY;
}

/*!
 * Get the callee function from an instruction
 */
const Function *ThreadAPI::getCallee(const Instruction *inst) const {
  if (const CallBase *cb = dyn_cast<CallBase>(inst)) {
    return cb->getCalledFunction();
  }
  return nullptr;
}

/*!
 * Get the callee function from a CallBase
 */
const Function *ThreadAPI::getCallee(const CallBase *cb) const {
  if (cb) {
    return cb->getCalledFunction();
  }
  return nullptr;
}

/*!
 * Get the CallBase from an instruction
 */
const CallBase *ThreadAPI::getLLVMCallSite(const Instruction *inst) const {
  return dyn_cast<CallBase>(inst);
}

/*!
 *
 */
void ThreadAPI::statInit(llvm::StringMap<u32_t> &tdAPIStatMap) {

  tdAPIStatMap["pthread_create"] = 0;

  tdAPIStatMap["pthread_join"] = 0;

  tdAPIStatMap["pthread_mutex_lock"] = 0;

  tdAPIStatMap["pthread_mutex_trylock"] = 0;

  tdAPIStatMap["pthread_mutex_unlock"] = 0;

  tdAPIStatMap["pthread_cancel"] = 0;

  tdAPIStatMap["pthread_exit"] = 0;

  tdAPIStatMap["pthread_detach"] = 0;

  tdAPIStatMap["pthread_cond_wait"] = 0;

  tdAPIStatMap["pthread_cond_signal"] = 0;

  tdAPIStatMap["pthread_cond_broadcast"] = 0;

  tdAPIStatMap["pthread_cond_init"] = 0;

  tdAPIStatMap["pthread_cond_destroy"] = 0;

  tdAPIStatMap["pthread_mutex_init"] = 0;

  tdAPIStatMap["pthread_mutex_destroy"] = 0;

  tdAPIStatMap["pthread_barrier_init"] = 0;

  tdAPIStatMap["pthread_barrier_wait"] = 0;

  tdAPIStatMap["hare_parallel_for"] = 0;
}

void ThreadAPI::performAPIStat(Module *module) {

  llvm::StringMap<u32_t> tdAPIStatMap;

  statInit(tdAPIStatMap);

  for (Module::iterator it = module->begin(), eit = module->end(); it != eit;
       ++it) {

    for (inst_iterator II = inst_begin(*it), E = inst_end(*it); II != E; ++II) {
      const Instruction *inst = &*II;
      if (!llvm::isa<CallInst>(inst) && !llvm::isa<InvokeInst>(inst))
        continue;
      const Function *fun = getCallee(inst);
      TD_TYPE type = getType(fun);
      switch (type) {
      case TD_FORK: {
        tdAPIStatMap["pthread_create"]++;
        break;
      }
      case TD_JOIN: {
        tdAPIStatMap["pthread_join"]++;
        break;
      }
      case TD_ACQUIRE: {
        tdAPIStatMap["pthread_mutex_lock"]++;
        break;
      }
      case TD_TRY_ACQUIRE: {
        tdAPIStatMap["pthread_mutex_trylock"]++;
        break;
      }
      case TD_RWLOCK_RDLOCK: {
        tdAPIStatMap["pthread_rwlock_rdlock"]++;
        break;
      }
      case TD_RWLOCK_WRLOCK: {
        tdAPIStatMap["pthread_rwlock_wrlock"]++;
        break;
      }
      case TD_RELEASE: {
        tdAPIStatMap["pthread_mutex_unlock"]++;
        break;
      }
      case TD_CANCEL: {
        tdAPIStatMap["pthread_cancel"]++;
        break;
      }
      case TD_EXIT: {
        tdAPIStatMap["pthread_exit"]++;
        break;
      }
      case TD_DETACH: {
        tdAPIStatMap["pthread_detach"]++;
        break;
      }
      case TD_COND_WAIT: {
        tdAPIStatMap["pthread_cond_wait"]++;
        break;
      }
      case TD_COND_SIGNAL: {
        tdAPIStatMap["pthread_cond_signal"]++;
        break;
      }
      case TD_COND_BROADCAST: {
        tdAPIStatMap["pthread_cond_broadcast"]++;
        break;
      }
      case TD_CONDVAR_INI: {
        tdAPIStatMap["pthread_cond_init"]++;
        break;
      }
      case TD_CONDVAR_DESTROY: {
        tdAPIStatMap["pthread_cond_destroy"]++;
        break;
      }
      case TD_MUTEX_INI: {
        tdAPIStatMap["pthread_mutex_init"]++;
        break;
      }
      case TD_MUTEX_DESTROY: {
        tdAPIStatMap["pthread_mutex_destroy"]++;
        break;
      }
      case TD_BAR_INIT: {
        tdAPIStatMap["pthread_barrier_init"]++;
        break;
      }
      case TD_BAR_WAIT: {
        tdAPIStatMap["pthread_barrier_wait"]++;
        break;
      }
      case HARE_PAR_FOR: {
        tdAPIStatMap["hare_parallel_for"]++;
        break;
      }
      case TD_DUMMY:
      default: {
        // Handle TD_DUMMY and all other thread API types (C++11/17/20, OpenMP, MPI, etc.)
        // These are not explicitly tracked in statistics
        break;
      }
      }
    }
  }

  StringRef n(module->getModuleIdentifier());
  StringRef name = n.split('/').second;
  name = name.split('.').first;
  std::string nameStr = name.str();
  std::cout << "################ (program : " << nameStr
            << ")###############\n";
  std::cout.flags(std::ios::left);
  unsigned field_width = 20;
  for (llvm::StringMap<u32_t>::iterator it = tdAPIStatMap.begin(),
                                        eit = tdAPIStatMap.end();
       it != eit; ++it) {
    std::string apiName = it->first().str();
    // format out put with width 20 space
    std::cout << std::setw(field_width) << apiName << " : " << it->second
              << "\n";
  }
  std::cout << "#######################################################"
            << "\n";
}

const char *ThreadAPI::tdTypeToString(TD_TYPE t) {
  switch (t) {
  case TD_DUMMY:              return "TD_DUMMY";
  case TD_FORK:               return "TD_FORK";
  case TD_JOIN:               return "TD_JOIN";
  case TD_DETACH:             return "TD_DETACH";
  case TD_ACQUIRE:            return "TD_ACQUIRE";
  case TD_TRY_ACQUIRE:        return "TD_TRY_ACQUIRE";
  case TD_RWLOCK_RDLOCK:      return "TD_RWLOCK_RDLOCK";
  case TD_RWLOCK_WRLOCK:      return "TD_RWLOCK_WRLOCK";
  case TD_RELEASE:            return "TD_RELEASE";
  case TD_EXIT:               return "TD_EXIT";
  case TD_CANCEL:             return "TD_CANCEL";
  case TD_COND_WAIT:          return "TD_COND_WAIT";
  case TD_COND_SIGNAL:        return "TD_COND_SIGNAL";
  case TD_COND_BROADCAST:     return "TD_COND_BROADCAST";
  case TD_MUTEX_INI:          return "TD_MUTEX_INI";
  case TD_MUTEX_DESTROY:      return "TD_MUTEX_DESTROY";
  case TD_CONDVAR_INI:        return "TD_CONDVAR_INI";
  case TD_CONDVAR_DESTROY:    return "TD_CONDVAR_DESTROY";
  case TD_BAR_INIT:           return "TD_BAR_INIT";
  case TD_BAR_WAIT:           return "TD_BAR_WAIT";
  case HARE_PAR_FOR:          return "HARE_PAR_FOR";
  case TD_SHARED_RDLOCK:      return "TD_SHARED_RDLOCK";
  case TD_SHARED_WRLOCK:      return "TD_SHARED_WRLOCK";
  case TD_SHARED_UNLOCK:      return "TD_SHARED_UNLOCK";
  case TD_CALL_ONCE:          return "TD_CALL_ONCE";
  case TD_FUTURE_GET:         return "TD_FUTURE_GET";
  case TD_FUTURE_WAIT:        return "TD_FUTURE_WAIT";
  case TD_PROMISE_SET:        return "TD_PROMISE_SET";
  case TD_ASYNC:              return "TD_ASYNC";
  case TD_LOCK_GUARD_CTOR:    return "TD_LOCK_GUARD_CTOR";
  case TD_LOCK_GUARD_DTOR:    return "TD_LOCK_GUARD_DTOR";
  case TD_UNIQUE_LOCK_CTOR:   return "TD_UNIQUE_LOCK_CTOR";
  case TD_UNIQUE_LOCK_DTOR:   return "TD_UNIQUE_LOCK_DTOR";
  case TD_UNIQUE_LOCK_LOCK:   return "TD_UNIQUE_LOCK_LOCK";
  case TD_UNIQUE_LOCK_UNLOCK: return "TD_UNIQUE_LOCK_UNLOCK";
  case TD_SCOPED_LOCK_CTOR:   return "TD_SCOPED_LOCK_CTOR";
  case TD_SCOPED_LOCK_DTOR:   return "TD_SCOPED_LOCK_DTOR";
  case TD_SHARED_LOCK_CTOR:   return "TD_SHARED_LOCK_CTOR";
  case TD_SHARED_LOCK_DTOR:   return "TD_SHARED_LOCK_DTOR";
  case TD_JTHREAD_FORK:       return "TD_JTHREAD_FORK";
  case TD_JTHREAD_JOIN:       return "TD_JTHREAD_JOIN";
  case TD_LATCH_COUNT_DOWN:   return "TD_LATCH_COUNT_DOWN";
  case TD_LATCH_WAIT:         return "TD_LATCH_WAIT";
  case TD_LATCH_ARRIVE_WAIT:  return "TD_LATCH_ARRIVE_WAIT";
  case TD_BARRIER_ARRIVE_WAIT: return "TD_BARRIER_ARRIVE_WAIT";
  case TD_BARRIER_ARRIVE:     return "TD_BARRIER_ARRIVE";
  case TD_BARRIER_WAIT_CPP20: return "TD_BARRIER_WAIT_CPP20";
  case TD_SEMAPHORE_ACQUIRE:  return "TD_SEMAPHORE_ACQUIRE";
  case TD_SEMAPHORE_RELEASE:  return "TD_SEMAPHORE_RELEASE";
  case TD_SEMAPHORE_TRY_ACQUIRE: return "TD_SEMAPHORE_TRY_ACQUIRE";
  case TD_OMP_TASK:           return "TD_OMP_TASK";
  case TD_OMP_TASKWAIT:       return "TD_OMP_TASKWAIT";
  case TD_OMP_TASKYIELD:      return "TD_OMP_TASKYIELD";
  case TD_OMP_TASKGROUP_START: return "TD_OMP_TASKGROUP_START";
  case TD_OMP_TASKGROUP_END:  return "TD_OMP_TASKGROUP_END";
  case TD_OMP_TASK_WITH_DEPS: return "TD_OMP_TASK_WITH_DEPS";
  case TD_OMP_TASKLOOP:       return "TD_OMP_TASKLOOP";
  case TD_OMP_SECTIONS_INIT:  return "TD_OMP_SECTIONS_INIT";
  case TD_OMP_SECTIONS_NEXT:  return "TD_OMP_SECTIONS_NEXT";
  case TD_OMP_SECTIONS_END:   return "TD_OMP_SECTIONS_END";
  case TD_OMP_ATOMIC_START:   return "TD_OMP_ATOMIC_START";
  case TD_OMP_ATOMIC_END:     return "TD_OMP_ATOMIC_END";
  case TD_OMP_FLUSH:          return "TD_OMP_FLUSH";
  case TD_OMP_CANCEL:         return "TD_OMP_CANCEL";
  case TD_OMP_TARGET:         return "TD_OMP_TARGET";
  case TD_OMP_TARGET_DATA_BEGIN: return "TD_OMP_TARGET_DATA_BEGIN";
  case TD_OMP_TARGET_DATA_END: return "TD_OMP_TARGET_DATA_END";
  case TD_MPI_INIT:           return "TD_MPI_INIT";
  case TD_MPI_FINALIZE:       return "TD_MPI_FINALIZE";
  case TD_MPI_SEND:           return "TD_MPI_SEND";
  case TD_MPI_RECV:           return "TD_MPI_RECV";
  case TD_MPI_SENDRECV:       return "TD_MPI_SENDRECV";
  case TD_MPI_PROBE:          return "TD_MPI_PROBE";
  case TD_MPI_ISEND:          return "TD_MPI_ISEND";
  case TD_MPI_IRECV:          return "TD_MPI_IRECV";
  case TD_MPI_IPROBE:         return "TD_MPI_IPROBE";
  case TD_MPI_WAIT:           return "TD_MPI_WAIT";
  case TD_MPI_WAITALL:        return "TD_MPI_WAITALL";
  case TD_MPI_WAITANY:        return "TD_MPI_WAITANY";
  case TD_MPI_WAITSOME:       return "TD_MPI_WAITSOME";
  case TD_MPI_TEST:           return "TD_MPI_TEST";
  case TD_MPI_TESTALL:        return "TD_MPI_TESTALL";
  case TD_MPI_TESTANY:        return "TD_MPI_TESTANY";
  case TD_MPI_TESTSOME:       return "TD_MPI_TESTSOME";
  case TD_MPI_BARRIER:        return "TD_MPI_BARRIER";
  case TD_MPI_BCAST:          return "TD_MPI_BCAST";
  case TD_MPI_SCATTER:        return "TD_MPI_SCATTER";
  case TD_MPI_GATHER:         return "TD_MPI_GATHER";
  case TD_MPI_ALLGATHER:      return "TD_MPI_ALLGATHER";
  case TD_MPI_ALLTOALL:       return "TD_MPI_ALLTOALL";
  case TD_MPI_REDUCE:         return "TD_MPI_REDUCE";
  case TD_MPI_ALLREDUCE:      return "TD_MPI_ALLREDUCE";
  case TD_MPI_REDUCE_SCATTER: return "TD_MPI_REDUCE_SCATTER";
  case TD_MPI_SCAN:           return "TD_MPI_SCAN";
  case TD_MPI_WIN_CREATE:     return "TD_MPI_WIN_CREATE";
  case TD_MPI_WIN_FREE:       return "TD_MPI_WIN_FREE";
  case TD_MPI_PUT:            return "TD_MPI_PUT";
  case TD_MPI_GET:            return "TD_MPI_GET";
  case TD_MPI_ACCUMULATE:     return "TD_MPI_ACCUMULATE";
  case TD_MPI_WIN_FENCE:      return "TD_MPI_WIN_FENCE";
  case TD_MPI_WIN_LOCK:       return "TD_MPI_WIN_LOCK";
  case TD_MPI_WIN_UNLOCK:     return "TD_MPI_WIN_UNLOCK";
  case TD_MPI_WIN_FLUSH:      return "TD_MPI_WIN_FLUSH";
  case TD_MPI_WIN_SYNC:       return "TD_MPI_WIN_SYNC";
  case TD_MPI_WIN_POST:       return "TD_MPI_WIN_POST";
  case TD_MPI_WIN_START:      return "TD_MPI_WIN_START";
  case TD_MPI_WIN_COMPLETE:   return "TD_MPI_WIN_COMPLETE";
  case TD_MPI_WIN_WAIT:       return "TD_MPI_WIN_WAIT";
  case TD_MPI_WIN_TEST:       return "TD_MPI_WIN_TEST";
  case TD_MPI_COMM_DUP:       return "TD_MPI_COMM_DUP";
  case TD_MPI_COMM_SPLIT:     return "TD_MPI_COMM_SPLIT";
  case TD_MPI_COMM_CREATE:    return "TD_MPI_COMM_CREATE";
  case TD_MPI_COMM_FREE:      return "TD_MPI_COMM_FREE";
  case TD_MPI_REQUEST_FREE:   return "TD_MPI_REQUEST_FREE";
  case TD_MPI_CANCEL:         return "TD_MPI_CANCEL";
  case TD_KERNEL_SPIN_LOCK_INIT: return "TD_KERNEL_SPIN_LOCK_INIT";
  case TD_KERNEL_SPIN_LOCK:   return "TD_KERNEL_SPIN_LOCK";
  case TD_KERNEL_SPIN_UNLOCK: return "TD_KERNEL_SPIN_UNLOCK";
  case TD_KERNEL_SPIN_TRYLOCK: return "TD_KERNEL_SPIN_TRYLOCK";
  case TD_KERNEL_MUTEX_INIT:  return "TD_KERNEL_MUTEX_INIT";
  case TD_KERNEL_MUTEX_LOCK:  return "TD_KERNEL_MUTEX_LOCK";
  case TD_KERNEL_MUTEX_UNLOCK: return "TD_KERNEL_MUTEX_UNLOCK";
  case TD_KERNEL_MUTEX_TRYLOCK: return "TD_KERNEL_MUTEX_TRYLOCK";
  case TD_KERNEL_SEMA_INIT:   return "TD_KERNEL_SEMA_INIT";
  case TD_KERNEL_DOWN:        return "TD_KERNEL_DOWN";
  case TD_KERNEL_UP:          return "TD_KERNEL_UP";
  case TD_KERNEL_READ_LOCK:   return "TD_KERNEL_READ_LOCK";
  case TD_KERNEL_READ_UNLOCK: return "TD_KERNEL_READ_UNLOCK";
  case TD_KERNEL_WRITE_LOCK:  return "TD_KERNEL_WRITE_LOCK";
  case TD_KERNEL_WRITE_UNLOCK: return "TD_KERNEL_WRITE_UNLOCK";
  case TD_KERNEL_DOWN_READ:   return "TD_KERNEL_DOWN_READ";
  case TD_KERNEL_UP_READ:     return "TD_KERNEL_UP_READ";
  case TD_KERNEL_DOWN_WRITE:  return "TD_KERNEL_DOWN_WRITE";
  case TD_KERNEL_UP_WRITE:    return "TD_KERNEL_UP_WRITE";
  case TD_KERNEL_INIT_RWSEM:  return "TD_KERNEL_INIT_RWSEM";
  case TD_KERNEL_RCU_READ_LOCK: return "TD_KERNEL_RCU_READ_LOCK";
  case TD_KERNEL_RCU_READ_UNLOCK: return "TD_KERNEL_RCU_READ_UNLOCK";
  case TD_KERNEL_SYNCHRONIZE_RCU: return "TD_KERNEL_SYNCHRONIZE_RCU";
  case TD_KERNEL_CALL_RCU:    return "TD_KERNEL_CALL_RCU";
  case TD_KERNEL_RCU_DEREFERENCE: return "TD_KERNEL_RCU_DEREFERENCE";
  case TD_KERNEL_RCU_ASSIGN_POINTER: return "TD_KERNEL_RCU_ASSIGN_POINTER";
  case TD_KERNEL_SEQLOCK_INIT: return "TD_KERNEL_SEQLOCK_INIT";
  case TD_KERNEL_READ_SEQBEGIN: return "TD_KERNEL_READ_SEQBEGIN";
  case TD_KERNEL_READ_SEQRETRY: return "TD_KERNEL_READ_SEQRETRY";
  case TD_KERNEL_WRITE_SEQLOCK: return "TD_KERNEL_WRITE_SEQLOCK";
  case TD_KERNEL_WRITE_SEQUNLOCK: return "TD_KERNEL_WRITE_SEQUNLOCK";
  case TD_KERNEL_INIT_COMPLETION: return "TD_KERNEL_INIT_COMPLETION";
  case TD_KERNEL_WAIT_FOR_COMPLETION: return "TD_KERNEL_WAIT_FOR_COMPLETION";
  case TD_KERNEL_COMPLETE:    return "TD_KERNEL_COMPLETE";
  case TD_KERNEL_INIT_WAITQUEUE_HEAD: return "TD_KERNEL_INIT_WAITQUEUE_HEAD";
  case TD_KERNEL_WAIT_EVENT:  return "TD_KERNEL_WAIT_EVENT";
  case TD_KERNEL_WAKE_UP:     return "TD_KERNEL_WAKE_UP";
  case TD_KERNEL_PREPARE_TO_WAIT: return "TD_KERNEL_PREPARE_TO_WAIT";
  case TD_KERNEL_FINISH_WAIT: return "TD_KERNEL_FINISH_WAIT";
  case TD_KERNEL_MEMORY_BARRIER: return "TD_KERNEL_MEMORY_BARRIER";
  default:                    return "<unknown TD_TYPE>";
  }
}
