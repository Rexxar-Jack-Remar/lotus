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

#include "Analysis/Concurrency/Utils/LanguageModel/Cpp11.h"
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

ThreadAPI *ThreadAPI::tdAPI = NULL;

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

  // 2. OpenMP Support
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

  // 3. C++11/17/20 Support
  // Basic threading
  if (Cpp11Model::isFork(name))
    return TD_FORK;
  if (Cpp11Model::isJoin(name))
    return TD_JOIN;
  if (Cpp11Model::isDetach(name))
    return TD_DETACH;
  
  // Basic mutex operations
  if (Cpp11Model::isAcquire(name))
    return TD_ACQUIRE;
  if (Cpp11Model::isTryAcquire(name))
    return TD_TRY_ACQUIRE;
  if (Cpp11Model::isRelease(name))
    return TD_RELEASE;
  
  // Condition variables
  if (Cpp11Model::isCondWait(name))
    return TD_COND_WAIT;
  if (Cpp11Model::isCondSignal(name))
    return TD_COND_SIGNAL;
  if (Cpp11Model::isCondBroadcast(name))
    return TD_COND_BROADCAST;
  
  // C++17 shared_mutex
  if (Cpp11Model::isSharedLockAcquire(name) || Cpp11Model::isSharedTimedLockAcquire(name))
    return TD_SHARED_RDLOCK;
  if (Cpp11Model::isSharedLockExclusiveAcquire(name) || Cpp11Model::isSharedTimedLockExclusiveAcquire(name))
    return TD_SHARED_WRLOCK;
  if (Cpp11Model::isSharedLockRelease(name) || Cpp11Model::isSharedLockExclusiveRelease(name))
    return TD_SHARED_UNLOCK;
  
  // RAII lock wrappers
  if (Cpp11Model::isLockGuardConstructor(name))
    return TD_LOCK_GUARD_CTOR;
  if (Cpp11Model::isLockGuardDestructor(name))
    return TD_LOCK_GUARD_DTOR;
  if (Cpp11Model::isUniqueLockConstructor(name))
    return TD_UNIQUE_LOCK_CTOR;
  if (Cpp11Model::isUniqueLockDestructor(name))
    return TD_UNIQUE_LOCK_DTOR;
  if (Cpp11Model::isUniqueLockLock(name))
    return TD_UNIQUE_LOCK_LOCK;
  if (Cpp11Model::isUniqueLockUnlock(name))
    return TD_UNIQUE_LOCK_UNLOCK;
  if (Cpp11Model::isScopedLockConstructor(name))
    return TD_SCOPED_LOCK_CTOR;
  if (Cpp11Model::isScopedLockDestructor(name))
    return TD_SCOPED_LOCK_DTOR;
  if (Cpp11Model::isSharedLockConstructor(name))
    return TD_SHARED_LOCK_CTOR;
  if (Cpp11Model::isSharedLockDestructor(name))
    return TD_SHARED_LOCK_DTOR;
  
  // std::call_once
  if (Cpp11Model::isCallOnce(name))
    return TD_CALL_ONCE;
  
  // Future/Promise synchronization
  if (Cpp11Model::isFutureGet(name))
    return TD_FUTURE_GET;
  if (Cpp11Model::isFutureWait(name))
    return TD_FUTURE_WAIT;
  if (Cpp11Model::isPromiseSetValue(name) || Cpp11Model::isPromiseSetException(name))
    return TD_PROMISE_SET;
  if (Cpp11Model::isAsync(name))
    return TD_ASYNC;
  
  // C++20 jthread
  if (Cpp11Model::isJthreadConstructor(name))
    return TD_JTHREAD_FORK;
  if (Cpp11Model::isJthreadJoin(name))
    return TD_JTHREAD_JOIN;
  
  // C++20 latch
  if (Cpp11Model::isLatchCountDown(name))
    return TD_LATCH_COUNT_DOWN;
  if (Cpp11Model::isLatchWait(name))
    return TD_LATCH_WAIT;
  if (Cpp11Model::isLatchArriveAndWait(name))
    return TD_LATCH_ARRIVE_WAIT;
  
  // C++20 barrier
  if (Cpp11Model::isBarrierArriveAndWait(name))
    return TD_BARRIER_ARRIVE_WAIT;
  if (Cpp11Model::isBarrierArrive(name))
    return TD_BARRIER_ARRIVE;
  if (Cpp11Model::isBarrierWait(name))
    return TD_BARRIER_WAIT_CPP20;
  
  // C++20 semaphore
  if (Cpp11Model::isSemaphoreAcquire(name))
    return TD_SEMAPHORE_ACQUIRE;
  if (Cpp11Model::isSemaphoreRelease(name))
    return TD_SEMAPHORE_RELEASE;
  if (Cpp11Model::isSemaphoreTryAcquire(name))
    return TD_SEMAPHORE_TRY_ACQUIRE;

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
      case TD_DUMMY: {
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