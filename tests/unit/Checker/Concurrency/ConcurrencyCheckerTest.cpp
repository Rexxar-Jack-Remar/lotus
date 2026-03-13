/**
 * @file ConcurrencyCheckerTest.cpp
 * @brief Unit tests for Concurrency checker
 *
 * The Concurrency checker detects concurrency-related bugs including:
 * - Data races
 * - Deadlocks
 * - Atomicity violations
 * - Lock mismatches
 */

#include "Checker/Concurrency/ConcurrencyChecker.h"

#include <gtest/gtest.h>
#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/SourceMgr.h>

using namespace llvm;

class ConcurrencyCheckerTest : public ::testing::Test {
protected:
  LLVMContext context;
  std::unique_ptr<Module> parseModule(const char *source) {
    SMDiagnostic err;
    auto module = parseAssemblyString(source, err, context);
    if (!module) {
      err.print("ConcurrencyCheckerTest", errs());
    }
    return module;
  }
};

// Test 1: Lock acquire/release pattern
TEST_F(ConcurrencyCheckerTest, LockAcquireRelease) {
  const char *source = R"(
    declare void @pthread_mutex_lock(i8*)
    declare void @pthread_mutex_unlock(i8*)
    
    define void @test_lock(i8* %mutex) {
      call void @pthread_mutex_lock(i8* %mutex)
      call void @pthread_mutex_unlock(i8* %mutex)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  Function *F = module->getFunction("test_lock");
  ASSERT_NE(F, nullptr);

  unsigned lockCount = 0, unlockCount = 0;
  for (auto &BB : *F) {
    for (auto &I : BB) {
      if (auto *CI = dyn_cast<CallInst>(&I)) {
        if (auto *callee = CI->getCalledFunction()) {
          StringRef name = callee->getName();
          if (name == "pthread_mutex_lock")
            ++lockCount;
          else if (name == "pthread_mutex_unlock")
            ++unlockCount;
        }
      }
    }
  }

  EXPECT_EQ(lockCount, 1u);
  EXPECT_EQ(unlockCount, 1u);
}

// Test 2: Thread creation
TEST_F(ConcurrencyCheckerTest, ThreadCreation) {
  const char *source = R"(
    declare i8* @thread_func(i8*)
    declare i32 @pthread_create(i8**, i8*, i8* (i8*)*, i8*)
    
    define i32 @test_thread_create() {
      %thread = alloca i8*
      %result = call i32 @pthread_create(i8** %thread, i8* null, i8* (i8*)* @thread_func, i8* null)
      ret i32 %result
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  Function *testFunc = module->getFunction("test_thread_create");
  Function *threadFunc = module->getFunction("thread_func");

  ASSERT_NE(testFunc, nullptr);
  ASSERT_NE(threadFunc, nullptr);

  CallInst *createCall = nullptr;
  for (auto &BB : *testFunc) {
    for (auto &I : BB) {
      if (auto *CI = dyn_cast<CallInst>(&I)) {
        if (CI->getCalledFunction() &&
            CI->getCalledFunction()->getName() == "pthread_create") {
          createCall = CI;
          break;
        }
      }
    }
  }

  ASSERT_NE(createCall, nullptr);
  EXPECT_EQ(createCall->arg_size(), 4u);
}

// Test 3: Shared variable access
TEST_F(ConcurrencyCheckerTest, SharedVariableAccess) {
  const char *source = R"(
    @shared_counter = global i32 0
    
    define void @increment() {
      %old = load i32, i32* @shared_counter
      %new = add i32 %old, 1
      store i32 %new, i32* @shared_counter
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  GlobalVariable *shared = module->getNamedGlobal("shared_counter");
  ASSERT_NE(shared, nullptr);

  Function *F = module->getFunction("increment");
  ASSERT_NE(F, nullptr);

  unsigned loadCount = 0, storeCount = 0;
  for (auto &BB : *F) {
    for (auto &I : BB) {
      if (isa<LoadInst>(&I))
        ++loadCount;
      if (isa<StoreInst>(&I))
        ++storeCount;
    }
  }

  EXPECT_EQ(loadCount, 1u);
  EXPECT_EQ(storeCount, 1u);
}

// Test 4: Lock order inconsistency
TEST_F(ConcurrencyCheckerTest, LockOrderInconsistency) {
  const char *source = R"(
    declare void @pthread_mutex_lock(i8*)
    declare void @pthread_mutex_unlock(i8*)
    
    define void @func1(i8* %mutex1, i8* %mutex2) {
      call void @pthread_mutex_lock(i8* %mutex1)
      call void @pthread_mutex_lock(i8* %mutex2)
      call void @pthread_mutex_unlock(i8* %mutex2)
      call void @pthread_mutex_unlock(i8* %mutex1)
      ret void
    }
    
    define void @func2(i8* %mutex1, i8* %mutex2) {
      call void @pthread_mutex_lock(i8* %mutex2)
      call void @pthread_mutex_lock(i8* %mutex1)
      call void @pthread_mutex_unlock(i8* %mutex1)
      call void @pthread_mutex_unlock(i8* %mutex2)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  Function *f1 = module->getFunction("func1");
  Function *f2 = module->getFunction("func2");

  ASSERT_NE(f1, nullptr);
  ASSERT_NE(f2, nullptr);

  bool f1HasLocks = false, f2HasLocks = false;
  for (auto *F : {f1, f2}) {
    for (auto &BB : *F) {
      for (auto &I : BB) {
        if (auto *CI = dyn_cast<CallInst>(&I)) {
          if (CI->getCalledFunction() &&
              CI->getCalledFunction()->getName() == "pthread_mutex_lock") {
            if (F == f1)
              f1HasLocks = true;
            else
              f2HasLocks = true;
          }
        }
      }
    }
  }

  EXPECT_TRUE(f1HasLocks);
  EXPECT_TRUE(f2HasLocks);
}

// Test 5: Atomic operation
TEST_F(ConcurrencyCheckerTest, AtomicOperation) {
  const char *source = R"(
    define i32 @atomic_fetch_add(i32* %ptr, i32 %val) {
      %result = atomicrmw add i32* %ptr, i32 %val monotonic
      ret i32 %result
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  Function *F = module->getFunction("atomic_fetch_add");
  ASSERT_NE(F, nullptr);

  AtomicRMWInst *atomicrmw = nullptr;
  for (auto &BB : *F) {
    for (auto &I : BB) {
      if (auto *ARMW = dyn_cast<AtomicRMWInst>(&I)) {
        atomicrmw = ARMW;
        break;
      }
    }
  }

  ASSERT_NE(atomicrmw, nullptr);
  EXPECT_EQ(atomicrmw->getOperation(), AtomicRMWInst::Add);
}

// Test 6: Thread join
TEST_F(ConcurrencyCheckerTest, ThreadJoin) {
  const char *source = R"(
    declare i32 @pthread_join(i8*, i8**)
    
    define i32 @test_join(i8* %thread) {
      %retval = alloca i8*
      %result = call i32 @pthread_join(i8* %thread, i8** %retval)
      ret i32 %result
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  Function *F = module->getFunction("test_join");
  ASSERT_NE(F, nullptr);

  CallInst *joinCall = nullptr;
  for (auto &BB : *F) {
    for (auto &I : BB) {
      if (auto *CI = dyn_cast<CallInst>(&I)) {
        if (CI->getCalledFunction() &&
            CI->getCalledFunction()->getName() == "pthread_join") {
          joinCall = CI;
          break;
        }
      }
    }
  }

  ASSERT_NE(joinCall, nullptr);
  EXPECT_EQ(joinCall->arg_size(), 2u);
}

// Test 7: Condition variable wait
TEST_F(ConcurrencyCheckerTest, ConditionVariable) {
  const char *source = R"(
    declare void @pthread_cond_wait(i8*, i8*)
    declare void @pthread_cond_signal(i8*)
    
    define void @test_cond_wait(i8* %cond, i8* %mutex) {
      call void @pthread_cond_wait(i8* %cond, i8* %mutex)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  Function *waitFunc = module->getFunction("test_cond_wait");
  ASSERT_NE(waitFunc, nullptr);

  EXPECT_FALSE(waitFunc->empty());
}

// Test 8: Once initialization
TEST_F(ConcurrencyCheckerTest, OnceInitialization) {
  const char *source = R"(
    declare void @pthread_once(i8*, i8*)
    
    define void @init_func() {
      ret void
    }
    
    define void @test_once(i8* %once) {
      call void @pthread_once(i8* %once, i8* bitcast (void ()* @init_func to i8*))
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  Function *testOnce = module->getFunction("test_once");
  Function *initFunc = module->getFunction("init_func");

  ASSERT_NE(testOnce, nullptr);
  ASSERT_NE(initFunc, nullptr);

  CallInst *onceCall = nullptr;
  for (auto &BB : *testOnce) {
    for (auto &I : BB) {
      if (auto *CI = dyn_cast<CallInst>(&I)) {
        if (CI->getCalledFunction() &&
            CI->getCalledFunction()->getName() == "pthread_once") {
          onceCall = CI;
          break;
        }
      }
    }
  }

  ASSERT_NE(onceCall, nullptr);
  EXPECT_EQ(onceCall->arg_size(), 2u);
}

// Test 9: Memory fence
TEST_F(ConcurrencyCheckerTest, MemoryFence) {
  const char *source = R"(
    declare void @llvm.arm.dmb(i8)
    
    define void @test_fence() {
      call void @llvm.arm.dmb(i8 0)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  Function *F = module->getFunction("test_fence");
  ASSERT_NE(F, nullptr);

  EXPECT_FALSE(F->empty());
}

// Test 10: Compare-and-swap
TEST_F(ConcurrencyCheckerTest, CompareAndSwap) {
  const char *source = R"(
    define void @test_cas(i32* %ptr) {
      %result = cmpxchg i32* %ptr, i32 0, i32 1 monotonic monotonic
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  Function *F = module->getFunction("test_cas");
  ASSERT_NE(F, nullptr);

  bool hasCmpXchg = false;
  for (auto &BB : *F) {
    for (auto &I : BB) {
      if (isa<AtomicCmpXchgInst>(&I)) {
        hasCmpXchg = true;
        break;
      }
    }
  }

  EXPECT_TRUE(hasCmpXchg);
}

TEST_F(ConcurrencyCheckerTest, DetectsOpenMPAtomicMismatch) {
  const char *source = R"(
    declare void @__kmpc_atomic_start()

    define void @test_openmp_atomic_mismatch() {
      call void @__kmpc_atomic_start()
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  concurrency::OpenMPChecker checker(*module, nullptr,
                                     ThreadAPI::getThreadAPI());
  auto reports = checker.checkOpenMPBugs();

  bool found = false;
  for (const auto &report : reports) {
    if (report.bugType ==
        concurrency::ConcurrencyBugType::OPENMP_ATOMIC_MISMATCH) {
      found = true;
      break;
    }
  }
  EXPECT_TRUE(found);
}

TEST_F(ConcurrencyCheckerTest, DetectsMPIOrphanedRequest) {
  const char *source = R"(
    declare i32 @MPI_Isend(i8*, i32, i32, i32, i32, i8*, i8**)

    define void @test_mpi_orphan(i8* %buf, i8* %comm, i8** %req) {
      %call = call i32 @MPI_Isend(i8* %buf, i32 1, i32 0, i32 1, i32 7, i8* %comm, i8** %req)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  concurrency::MPIChecker checker(*module);
  auto reports = checker.checkMPIBugs();

  bool found = false;
  for (const auto &report : reports) {
    if (report.bugType ==
        concurrency::ConcurrencyBugType::MPI_ORPHANED_REQUEST) {
      found = true;
      break;
    }
  }
  EXPECT_TRUE(found);
}

TEST_F(ConcurrencyCheckerTest, TracksOpenMPSummaryInCheckerStatistics) {
  const char *source = R"(
    %kmp_depend_info = type { i8*, i64, i8 }
    @shared = global i32 0, align 4
    @deps = global [1 x %kmp_depend_info] [
      %kmp_depend_info { i8* bitcast (i32* @shared to i8*), i64 4, i8 2 }
    ]

    declare i32 @__kmpc_omp_task_with_deps(i8*, i32, i8*, i32,
                                           %kmp_depend_info*, i32,
                                           %kmp_depend_info*)
    declare i32 @__kmpc_omp_wait_deps(i8*, i32, i32, i8*, i32, i8*)
    declare i32 @__kmpc_taskloop(i8*, i32, i8*, i32, i64*, i64, i32, i32, i64)
    declare void @__kmpc_taskgroup(i8*, i32)
    declare i32 @__kmpc_atomic_start()
    declare i32 @__kmpc_atomic_end()
    declare i32 @__kmpc_flush(i8*)

    define i32 @main() {
    entry:
      call void @__kmpc_taskgroup(i8* null, i32 0)
      %dep = getelementptr inbounds [1 x %kmp_depend_info],
              [1 x %kmp_depend_info]* @deps, i64 0, i64 0
      call i32 @__kmpc_omp_task_with_deps(i8* null, i32 0, i8* null, i32 1,
                                          %kmp_depend_info* %dep, i32 0, %kmp_depend_info* null)
      call i32 @__kmpc_omp_wait_deps(i8* null, i32 0, i32 0, i8* null, i32 0, i8* null)
      call i32 @__kmpc_taskloop(i8* null, i32 0, i8* null, i32 0,
                                i64* null, i64 0, i32 0, i32 0, i64 0)
      call i32 @__kmpc_atomic_start()
      call i32 @__kmpc_atomic_end()
      call i32 @__kmpc_flush(i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  concurrency::ConcurrencyChecker checker(*module);
  checker.enableDataRaceCheck(false);
  checker.enableDeadlockCheck(false);
  checker.enableAtomicityCheck(false);
  checker.enableCondVarCheck(false);
  checker.enableLockMismatchCheck(false);
  checker.enableMPICheck(false);
  checker.enableOpenMPCheck(true);
  checker.runAnalyses();

  auto stats = checker.getStatistics();
  EXPECT_EQ(stats.openMPSummary.task_count, 2u);
  EXPECT_EQ(stats.openMPSummary.task_with_dependencies_count, 1u);
  EXPECT_EQ(stats.openMPSummary.taskloop_count, 1u);
  EXPECT_EQ(stats.openMPSummary.partial_wait_boundary_count, 1u);
  EXPECT_EQ(stats.openMPSummary.taskgroup_region_count, 1u);
  EXPECT_EQ(stats.openMPSummary.atomic_region_count, 1u);
  EXPECT_EQ(stats.openMPSummary.flush_count, 1u);
}

TEST_F(ConcurrencyCheckerTest, ConditionalMayLockDoesNotSuppressRealRace) {
  const char *source = R"(
    @lock = global i8 0
    @shared = global i32 0, align 4
    @flag = external global i1

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare i32 @pthread_mutex_lock(i8*)

    define i8* @worker1(i8* %arg) {
    entry:
      %cond = load i1, i1* @flag
      br i1 %cond, label %locked, label %merge

    locked:
      call i32 @pthread_mutex_lock(i8* @lock)
      br label %merge

    merge:
      store i32 1, i32* @shared, align 4
      ret i8* null
    }

    define i8* @worker2(i8* %arg) {
    entry:
      %cond = load i1, i1* @flag
      br i1 %cond, label %locked, label %merge

    locked:
      call i32 @pthread_mutex_lock(i8* @lock)
      br label %merge

    merge:
      store i32 2, i32* @shared, align 4
      ret i8* null
    }

    define i32 @main() {
    entry:
      %tid1 = alloca i8, align 1
      %tid2 = alloca i8, align 1
      call i32 @pthread_create(i8* %tid1, i8* null, i8* (i8*)* @worker1, i8* null)
      call i32 @pthread_create(i8* %tid2, i8* null, i8* (i8*)* @worker2, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  mhp::MHPAnalysis mhp(*module);
  mhp.enableLockSetAnalysis();
  mhp.analyze();

  lotus::EscapeAnalysis escape(*module);
  escape.analyze();

  concurrency::DataRaceChecker checker(*module, &mhp, mhp.getLockSetAnalysis(),
                                       &escape, mhp.getAliasAnalysis(),
                                       nullptr);

  const Instruction *store1 = nullptr;
  const Instruction *store2 = nullptr;
  for (const Instruction &inst :
       instructions(*module->getFunction("worker1"))) {
    if (isa<StoreInst>(&inst)) {
      store1 = &inst;
      break;
    }
  }
  for (const Instruction &inst :
       instructions(*module->getFunction("worker2"))) {
    if (isa<StoreInst>(&inst)) {
      store2 = &inst;
      break;
    }
  }

  ASSERT_NE(store1, nullptr);
  ASSERT_NE(store2, nullptr);
  EXPECT_TRUE(checker.wouldReportDataRace(store1, store2));
}

TEST_F(ConcurrencyCheckerTest, TracksMPISummaryInCheckerStatistics) {
  const char *source = R"(
    @win = global i8 0, align 1
    declare i32 @MPI_Isend(i8*, i32, i32, i32, i32, i8*, i8*)
    declare i32 @MPI_Ibarrier(i8*, i8*)
    declare i32 @MPI_Request_free(i8*)
    declare i32 @MPI_Win_create(i8*, i64, i32, i8*, i8*)
    declare i32 @MPI_Win_lock(i32, i32, i32, i8*)
    declare i32 @MPI_Put(i8*, i32, i32, i32, i64, i32, i32, i8*)
    declare i32 @MPI_Win_unlock(i32, i8*)

    define i32 @main(i8* %comm) {
    entry:
      %req = alloca i8, align 1
      call i32 @MPI_Isend(i8* null, i32 1, i32 0, i32 1, i32 7, i8* %comm, i8* %req)
      call i32 @MPI_Ibarrier(i8* %comm, i8* %req)
      call i32 @MPI_Request_free(i8* %req)
      call i32 @MPI_Win_create(i8* null, i64 8, i32 4, i8* %comm, i8* @win)
      call i32 @MPI_Win_lock(i32 0, i32 1, i32 0, i8* @win)
      call i32 @MPI_Put(i8* null, i32 1, i32 0, i32 1, i64 0, i32 1, i32 0, i8* @win)
      call i32 @MPI_Win_unlock(i32 1, i8* @win)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  concurrency::ConcurrencyChecker checker(*module);
  checker.enableDataRaceCheck(false);
  checker.enableDeadlockCheck(false);
  checker.enableAtomicityCheck(false);
  checker.enableCondVarCheck(false);
  checker.enableLockMismatchCheck(false);
  checker.enableOpenMPCheck(false);
  checker.enableMPICheck(true);
  checker.runAnalyses();

  auto stats = checker.getStatistics();
  EXPECT_EQ(stats.mpiSummary.operation_count, 7u);
  EXPECT_EQ(stats.mpiSummary.nonblocking_operation_count, 2u);
  EXPECT_EQ(stats.mpiSummary.collective_operation_count, 1u);
  EXPECT_EQ(stats.mpiSummary.request_management_count, 1u);
  EXPECT_EQ(stats.mpiSummary.rma_operation_count, 1u);
  EXPECT_EQ(stats.mpiSummary.rma_sync_count, 2u);
  EXPECT_EQ(stats.mpiSummary.leaked_window_count, 1u);
  EXPECT_EQ(stats.mpiSummary.collective_slot_count, 1u);
}

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
