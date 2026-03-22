#include "Analysis/Concurrency/Utils/ThreadAPI.h"

#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/SourceMgr.h>
#include <gtest/gtest.h>

using namespace llvm;

class ThreadAPITest : public ::testing::Test {
protected:
  LLVMContext context;

  std::unique_ptr<Module> parseModule(const char *source) {
    SMDiagnostic err;
    auto module = parseAssemblyString(source, err, context);
    if (!module) {
      err.print("ThreadAPITest", errs());
    }
    return module;
  }
};

TEST_F(ThreadAPITest, ParsesExtendedTypeNames) {
  EXPECT_EQ(ThreadAPI::stringToType("TD_CANCEL"), ThreadAPI::TD_CANCEL);
  EXPECT_EQ(ThreadAPI::stringToType("TD_MPI_BARRIER"),
            ThreadAPI::TD_MPI_BARRIER);
  EXPECT_EQ(ThreadAPI::stringToType("TD_SHARED_LOCK_DTOR"),
            ThreadAPI::TD_SHARED_LOCK_DTOR);
  EXPECT_EQ(ThreadAPI::stringToType("TD_OMP_TASKWAIT_DEPS"),
            ThreadAPI::TD_OMP_TASKWAIT_DEPS);
  EXPECT_EQ(ThreadAPI::stringToType("TD_OMP_SINGLE_END"),
            ThreadAPI::TD_OMP_SINGLE_END);
  EXPECT_EQ(ThreadAPI::stringToType("TD_OMP_ORDERED_START"),
            ThreadAPI::TD_OMP_ORDERED_START);
  EXPECT_EQ(ThreadAPI::stringToType("TD_OMP_TARGET_DATA_UPDATE"),
            ThreadAPI::TD_OMP_TARGET_DATA_UPDATE);
  EXPECT_EQ(ThreadAPI::stringToType("TD_MPI_PERSISTENT_SEND_INIT"),
            ThreadAPI::TD_MPI_PERSISTENT_SEND_INIT);
  EXPECT_EQ(ThreadAPI::stringToType("TD_MPI_PERSISTENT_RECV_INIT"),
            ThreadAPI::TD_MPI_PERSISTENT_RECV_INIT);
  EXPECT_EQ(ThreadAPI::stringToType("TD_MPI_REQUEST_START"),
            ThreadAPI::TD_MPI_REQUEST_START);
}

TEST_F(ThreadAPITest, PthreadCancelIsNotClassifiedAsJoin) {
  const char *source = R"(
    declare i32 @pthread_cancel(i8*)

    define i32 @main(i8* %tid) {
    entry:
      %cancel = call i32 @pthread_cancel(i8* %tid)
      ret i32 %cancel
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  const Function *cancel_func = module->getFunction("pthread_cancel");
  ASSERT_NE(cancel_func, nullptr);
  EXPECT_EQ(api->getType(cancel_func), ThreadAPI::TD_CANCEL);

  const Function *main_func = module->getFunction("main");
  ASSERT_NE(main_func, nullptr);
  const Instruction *cancel_call = &main_func->getEntryBlock().front();
  EXPECT_FALSE(api->isTDJoin(cancel_call));
}

TEST_F(ThreadAPITest, DistinguishesBlockingAndNonBlockingMPICollectives) {
  const char *source = R"(
    declare i32 @MPI_Barrier(i8*)
    declare i32 @MPI_Ibarrier(i8*, i8*)
    declare i32 @MPI_Bcast(i8*, i32, i32, i32, i8*)
    declare i32 @MPI_Ibcast(i8*, i32, i32, i32, i8*, i8*)

    define i32 @main(i8* %comm, i8* %req) {
    entry:
      %bar = call i32 @MPI_Barrier(i8* %comm)
      %ibar = call i32 @MPI_Ibarrier(i8* %comm, i8* %req)
      %bcast = call i32 @MPI_Bcast(i8* null, i32 0, i32 0, i32 0, i8* %comm)
      %ibcast = call i32 @MPI_Ibcast(i8* null, i32 0, i32 0, i32 0, i8* %comm, i8* %req)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  const Function *main_func = module->getFunction("main");
  ASSERT_NE(main_func, nullptr);

  auto it = main_func->getEntryBlock().begin();
  const Instruction *bar = &*it++;
  const Instruction *ibar = &*it++;
  const Instruction *bcast = &*it++;
  const Instruction *ibcast = &*it++;

  EXPECT_TRUE(api->isBlockingMPIBarrier(bar));
  EXPECT_TRUE(api->isNonBlockingMPIBarrier(ibar));
  EXPECT_TRUE(api->isBlockingMPICollective(bcast));
  EXPECT_TRUE(api->isNonBlockingMPICollective(ibcast));
}

TEST_F(ThreadAPITest, NormalizesPMPIAliasesForMPIClassification) {
  const char *source = R"(
    declare i32 @PMPI_Ibarrier(i8*, i8*)
    declare i32 @PMPI_Ibcast(i8*, i32, i32, i32, i8*, i8*)

    define i32 @main(i8* %comm, i8* %req) {
    entry:
      %ibar = call i32 @PMPI_Ibarrier(i8* %comm, i8* %req)
      %ibcast = call i32 @PMPI_Ibcast(i8* null, i32 0, i32 0, i32 0, i8* %comm, i8* %req)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  const Function *main_func = module->getFunction("main");
  ASSERT_NE(main_func, nullptr);

  auto it = main_func->getEntryBlock().begin();
  const Instruction *ibar = &*it++;
  const Instruction *ibcast = &*it++;

  EXPECT_EQ(api->getType(module->getFunction("PMPI_Ibarrier")),
            ThreadAPI::TD_MPI_BARRIER);
  EXPECT_EQ(api->getType(module->getFunction("PMPI_Ibcast")),
            ThreadAPI::TD_MPI_BCAST);
  EXPECT_TRUE(api->isNonBlockingMPIBarrier(ibar));
  EXPECT_TRUE(api->isNonBlockingMPICollective(ibcast));
}

TEST_F(ThreadAPITest, MatchesSpecificOpenMPTargetDataBeforeGenericTarget) {
  const char *source = R"(
    declare void @__tgt_target_data_begin(i64, i8*)
    declare void @__tgt_target_data_end(i64, i8*)

    define void @main() {
    entry:
      call void @__tgt_target_data_begin(i64 0, i8* null)
      call void @__tgt_target_data_end(i64 0, i8* null)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  EXPECT_EQ(api->getType(module->getFunction("__tgt_target_data_begin")),
            ThreadAPI::TD_OMP_TARGET_DATA_BEGIN);
  EXPECT_EQ(api->getType(module->getFunction("__tgt_target_data_end")),
            ThreadAPI::TD_OMP_TARGET_DATA_END);
}

TEST_F(ThreadAPITest, RecognizesExtendedOpenMPTargetDataVariantsAndHelpers) {
  const char *source = R"(
    declare void @__tgt_target_data_update(i64, i8*)
    declare void @__tgt_target_enter_data(i64, i8*)
    declare void @__tgt_target_exit_data(i64, i8*)

    define void @main() {
    entry:
      call void @__tgt_target_data_update(i64 0, i8* null)
      call void @__tgt_target_enter_data(i64 0, i8* null)
      call void @__tgt_target_exit_data(i64 0, i8* null)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  EXPECT_EQ(api->getType(module->getFunction("__tgt_target_data_update")),
            ThreadAPI::TD_OMP_TARGET_DATA_UPDATE);
  EXPECT_EQ(api->getType(module->getFunction("__tgt_target_enter_data")),
            ThreadAPI::TD_OMP_TARGET_DATA_BEGIN);
  EXPECT_EQ(api->getType(module->getFunction("__tgt_target_exit_data")),
            ThreadAPI::TD_OMP_TARGET_DATA_END);

  const Function *main_func = module->getFunction("main");
  ASSERT_NE(main_func, nullptr);
  auto it = main_func->getEntryBlock().begin();
  const Instruction *update = &*it++;
  const Instruction *enter = &*it++;
  const Instruction *exit = &*it++;

  EXPECT_TRUE(api->isOMPTargetOp(update));
  EXPECT_TRUE(api->isOMPTargetDataOp(update));
  EXPECT_TRUE(api->isOMPTargetDataOp(enter));
  EXPECT_TRUE(api->isOMPTargetDataOp(exit));
}

TEST_F(ThreadAPITest, RecognizesOpenMPLockLifecycleAndTryLockRoutines) {
  const char *source = R"(
    declare void @omp_init_lock(i8*)
    declare i32 @omp_test_lock(i8*)
    declare void @omp_destroy_lock(i8*)
    declare void @omp_init_nest_lock(i8*)
    declare i32 @omp_test_nest_lock(i8*)
    declare void @omp_destroy_nest_lock(i8*)

    define void @main(i8* %lock, i8* %nest) {
    entry:
      call void @omp_init_lock(i8* %lock)
      call i32 @omp_test_lock(i8* %lock)
      call void @omp_destroy_lock(i8* %lock)
      call void @omp_init_nest_lock(i8* %nest)
      call i32 @omp_test_nest_lock(i8* %nest)
      call void @omp_destroy_nest_lock(i8* %nest)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  EXPECT_EQ(api->getType(module->getFunction("omp_init_lock")),
            ThreadAPI::TD_MUTEX_INI);
  EXPECT_EQ(api->getType(module->getFunction("omp_test_lock")),
            ThreadAPI::TD_TRY_ACQUIRE);
  EXPECT_EQ(api->getType(module->getFunction("omp_destroy_lock")),
            ThreadAPI::TD_MUTEX_DESTROY);
  EXPECT_EQ(api->getType(module->getFunction("omp_init_nest_lock")),
            ThreadAPI::TD_MUTEX_INI);
  EXPECT_EQ(api->getType(module->getFunction("omp_test_nest_lock")),
            ThreadAPI::TD_TRY_ACQUIRE);
  EXPECT_EQ(api->getType(module->getFunction("omp_destroy_nest_lock")),
            ThreadAPI::TD_MUTEX_DESTROY);

  const Function *main_func = module->getFunction("main");
  ASSERT_NE(main_func, nullptr);
  auto it = main_func->getEntryBlock().begin();
  ++it;
  const Instruction *test_lock = &*it++;
  ++it;
  ++it;
  const Instruction *test_nest_lock = &*it;
  EXPECT_TRUE(api->isTryLock(test_lock));
  EXPECT_TRUE(api->isTryLock(test_nest_lock));
}

TEST_F(ThreadAPITest, DefaultSemaphoresAreNotLockExclusionOps) {
  const char *source = R"(
    declare i32 @sem_wait(i8*)
    declare i32 @sem_post(i8*)
    declare void @fake_counting_semaphore_acquireEv(i8*)
    declare void @fake_counting_semaphore_releaseEv(i8*)

    define void @main(i8* %sem) {
    entry:
      call i32 @sem_wait(i8* %sem)
      call i32 @sem_post(i8* %sem)
      call void @fake_counting_semaphore_acquireEv(i8* %sem)
      call void @fake_counting_semaphore_releaseEv(i8* %sem)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  const Function *main_func = module->getFunction("main");
  ASSERT_NE(main_func, nullptr);

  auto it = main_func->getEntryBlock().begin();
  const Instruction *sem_wait_call = &*it++;
  const Instruction *sem_post_call = &*it++;
  const Instruction *cpp_acquire = &*it++;
  const Instruction *cpp_release = &*it++;

  EXPECT_TRUE(api->isSemaphoreOp(sem_wait_call));
  EXPECT_TRUE(api->isSemaphoreOp(sem_post_call));
  EXPECT_TRUE(api->isSemaphoreOp(cpp_acquire));
  EXPECT_TRUE(api->isSemaphoreOp(cpp_release));

  EXPECT_FALSE(api->isBinarySemaphoreOp(sem_wait_call));
  EXPECT_FALSE(api->isBinarySemaphoreOp(cpp_acquire));
  EXPECT_FALSE(api->isTDAcquire(sem_wait_call));
  EXPECT_FALSE(api->isTDRelease(sem_post_call));
  EXPECT_FALSE(api->isTDAcquire(cpp_acquire));
  EXPECT_FALSE(api->isTDRelease(cpp_release));
}

TEST_F(ThreadAPITest, ConfigTaggedBinarySemaphoresRemainExclusionCapable) {
  const char *source = R"(
    declare i32 @binary_sem_wait(i8*)
    declare i32 @binary_sem_post(i8*)

    define void @main(i8* %sem) {
    entry:
      call i32 @binary_sem_wait(i8* %sem)
      call i32 @binary_sem_post(i8* %sem)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  const Function *main_func = module->getFunction("main");
  ASSERT_NE(main_func, nullptr);

  auto it = main_func->getEntryBlock().begin();
  const Instruction *binary_wait = &*it++;
  const Instruction *binary_post = &*it++;

  EXPECT_TRUE(api->isSemaphoreOp(binary_wait));
  EXPECT_TRUE(api->isSemaphoreOp(binary_post));
  EXPECT_TRUE(api->isBinarySemaphoreOp(binary_wait));
  EXPECT_TRUE(api->isBinarySemaphoreOp(binary_post));
  EXPECT_TRUE(api->isTDAcquire(binary_wait));
  EXPECT_TRUE(api->isTDRelease(binary_post));
}

TEST_F(ThreadAPITest, RecognizesAdditionalMPICommunicatorManagementAPIs) {
  const char *source = R"(
    declare i32 @MPI_Intercomm_create(i8*, i32, i8*, i32, i32, i8**)
    declare i32 @MPI_Intercomm_merge(i8*, i32, i8**)
    declare i32 @MPI_Comm_disconnect(i8**)
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  EXPECT_EQ(api->getType(module->getFunction("MPI_Intercomm_create")),
            ThreadAPI::TD_MPI_COMM_CREATE);
  EXPECT_EQ(api->getType(module->getFunction("MPI_Intercomm_merge")),
            ThreadAPI::TD_MPI_COMM_CREATE);
  EXPECT_EQ(api->getType(module->getFunction("MPI_Comm_disconnect")),
            ThreadAPI::TD_MPI_COMM_FREE);
}

TEST_F(ThreadAPITest, RecognizesPersistentMPIRequestLifecycleHelpers) {
  const char *source = R"(
    declare i32 @MPI_Send_init(i8*, i32, i32, i32, i32, i8*, i8*)
    declare i32 @MPI_Recv_init(i8*, i32, i32, i32, i32, i8*, i8*)
    declare i32 @MPI_Start(i8*)

    define i32 @main(i8* %comm, i8* %req1, i8* %req2) {
    entry:
      call i32 @MPI_Send_init(i8* null, i32 1, i32 0, i32 1, i32 7, i8* %comm, i8* %req1)
      call i32 @MPI_Recv_init(i8* null, i32 1, i32 0, i32 1, i32 7, i8* %comm, i8* %req2)
      call i32 @MPI_Start(i8* %req1)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  EXPECT_EQ(api->getType(module->getFunction("MPI_Send_init")),
            ThreadAPI::TD_MPI_PERSISTENT_SEND_INIT);
  EXPECT_EQ(api->getType(module->getFunction("MPI_Recv_init")),
            ThreadAPI::TD_MPI_PERSISTENT_RECV_INIT);
  EXPECT_EQ(api->getType(module->getFunction("MPI_Start")),
            ThreadAPI::TD_MPI_REQUEST_START);

  const Function *main_func = module->getFunction("main");
  ASSERT_NE(main_func, nullptr);
  auto it = main_func->getEntryBlock().begin();
  const Instruction *send_init = &*it++;
  const Instruction *recv_init = &*it++;
  const Instruction *start = &*it++;

  EXPECT_TRUE(api->isMPIRequestManagement(send_init));
  EXPECT_TRUE(api->isMPIRequestManagement(recv_init));
  EXPECT_TRUE(api->isMPIRequestManagement(start));
  EXPECT_TRUE(api->isPersistentMPIRequestInit(send_init));
  EXPECT_TRUE(api->isPersistentMPIRequestInit(recv_init));
  EXPECT_TRUE(api->isPersistentMPIRequestStart(start));
}

TEST_F(ThreadAPITest, RecognizesJthreadAndTreatsItAsForkLike) {
  const char *source = R"(
    declare void @_ZNSt7jthreadC1EPFvPvES0_(i8*, i8* (i8*)*, i8*)
    declare void @_ZNSt7jthread4joinEv(i8*)

    define i8* @worker(i8* %arg) {
    entry:
      ret i8* null
    }

    define void @main() {
    entry:
      %thr = alloca i8
      call void @_ZNSt7jthreadC1EPFvPvES0_(i8* %thr, i8* (i8*)* @worker, i8* null)
      call void @_ZNSt7jthread4joinEv(i8* %thr)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();

  const Function *main_func = module->getFunction("main");
  ASSERT_NE(main_func, nullptr);
  const Instruction *fork = nullptr;
  const Instruction *join = nullptr;
  for (const Instruction &inst : main_func->getEntryBlock()) {
    if (isa<CallBase>(&inst)) {
      if (!fork) {
        fork = &inst;
      } else if (!join) {
        join = &inst;
        break;
      }
    }
  }
  ASSERT_NE(fork, nullptr);
  ASSERT_NE(join, nullptr);

  EXPECT_EQ(api->getType(module->getFunction("_ZNSt7jthreadC1EPFvPvES0_")),
            ThreadAPI::TD_JTHREAD_FORK);
  EXPECT_EQ(api->getType(module->getFunction("_ZNSt7jthread4joinEv")),
            ThreadAPI::TD_JTHREAD_JOIN);
  EXPECT_TRUE(api->isTDFork(fork));
  EXPECT_TRUE(api->isTDJoin(join));
}

TEST_F(ThreadAPITest, StdThreadMoveConstructorIsNotFork) {
  const char *source = R"(
    declare void @_ZNSt6threadC1EOS_(i8*, i8*)

    define void @main() {
    entry:
      %dst = alloca i8
      %src = alloca i8
      call void @_ZNSt6threadC1EOS_(i8* %dst, i8* %src)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  const Function *move_ctor = module->getFunction("_ZNSt6threadC1EOS_");
  ASSERT_NE(move_ctor, nullptr);
  EXPECT_EQ(api->getType(move_ctor), ThreadAPI::TD_DUMMY);

  const Function *main_func = module->getFunction("main");
  ASSERT_NE(main_func, nullptr);
  const Instruction *call = &main_func->getEntryBlock().front();
  EXPECT_FALSE(api->isTDFork(call));
}

TEST_F(ThreadAPITest, RecognizesLibcxxJoinDetachManglings) {
  const char *source = R"(
    declare void @_ZNSt3__16thread4joinEv(i8*)
    declare void @_ZNSt3__16thread6detachEv(i8*)
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  EXPECT_EQ(api->getType(module->getFunction("_ZNSt3__16thread4joinEv")),
            ThreadAPI::TD_JOIN);
  EXPECT_EQ(api->getType(module->getFunction("_ZNSt3__16thread6detachEv")),
            ThreadAPI::TD_DETACH);
}

TEST_F(ThreadAPITest, StdJthreadMoveConstructorIsNotFork) {
  const char *source = R"(
    declare void @_ZNSt7jthreadC1EOS_(i8*, i8*)

    define void @main() {
    entry:
      %dst = alloca i8
      %src = alloca i8
      call void @_ZNSt7jthreadC1EOS_(i8* %dst, i8* %src)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  const Function *move_ctor = module->getFunction("_ZNSt7jthreadC1EOS_");
  ASSERT_NE(move_ctor, nullptr);
  EXPECT_EQ(api->getType(move_ctor), ThreadAPI::TD_DUMMY);
}

TEST_F(ThreadAPITest, MapsOpenMPTaskwaitWithDepsVariants) {
  const char *source = R"(
    declare i32 @__kmpc_omp_taskwait(i8*, i32)
    declare i32 @__kmpc_omp_wait_deps(i8*, i32, i32, i8*, i32, i8*)
    declare i32 @__kmpc_omp_taskwait_deps_51(i8*, i32, i32, i8*, i32, i8*, i8*)
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  EXPECT_EQ(api->getType(module->getFunction("__kmpc_omp_taskwait")),
            ThreadAPI::TD_OMP_TASKWAIT);
  EXPECT_EQ(api->getType(module->getFunction("__kmpc_omp_wait_deps")),
            ThreadAPI::TD_OMP_TASKWAIT_DEPS);
  EXPECT_EQ(api->getType(module->getFunction("__kmpc_omp_taskwait_deps_51")),
            ThreadAPI::TD_OMP_TASKWAIT_DEPS);
}

TEST_F(ThreadAPITest, ClassifiesSharedTimedMutexReleases) {
  const char *source = R"(
    declare void @_ZNSt18shared_timed_mutex13unlock_sharedEv(i8*)
    declare void @_ZNSt18shared_timed_mutex6unlockEv(i8*)
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  EXPECT_EQ(api->getType(module->getFunction(
                "_ZNSt18shared_timed_mutex13unlock_sharedEv")),
            ThreadAPI::TD_SHARED_UNLOCK);
  EXPECT_EQ(
      api->getType(module->getFunction("_ZNSt18shared_timed_mutex6unlockEv")),
      ThreadAPI::TD_SHARED_UNLOCK);
}

TEST_F(ThreadAPITest, ExtractsOutlinedOpenMPForkTarget) {
  const char *source = R"(
    declare void @__kmpc_fork_call(i8*, i32, void (i32*, i32*, ...)*)

    define internal void @.omp_outlined.(i32* %gtid, i32* %btid, ...) {
    entry:
      ret void
    }

    define i32 @main() {
    entry:
      call void @__kmpc_fork_call(i8* null, i32 0,
                                  void (i32*, i32*, ...)* @.omp_outlined.)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  const Function *main_func = module->getFunction("main");
  ASSERT_NE(main_func, nullptr);
  const Instruction *fork = &main_func->getEntryBlock().front();
  EXPECT_TRUE(api->isTDFork(fork));
  EXPECT_EQ(api->getForkedFun(fork), module->getFunction(".omp_outlined."));
}

TEST_F(ThreadAPITest, SharedLockPredicatesAreConsistentAcrossOverloads) {
  const char *source = R"(
    declare void @_ZNSt12shared_mutex11lock_sharedEv(i8*)
    declare void @_ZNSt12shared_mutex4lockEv(i8*)

    define void @main(i8* %m) {
    entry:
      call void @_ZNSt12shared_mutex11lock_sharedEv(i8* %m)
      call void @_ZNSt12shared_mutex4lockEv(i8* %m)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  const Function *main_func = module->getFunction("main");
  ASSERT_NE(main_func, nullptr);

  auto it = main_func->getEntryBlock().begin();
  const auto *shared_call = llvm::dyn_cast<CallBase>(&*it++);
  const auto *exclusive_call = llvm::dyn_cast<CallBase>(&*it++);
  ASSERT_NE(shared_call, nullptr);
  ASSERT_NE(exclusive_call, nullptr);

  EXPECT_EQ(api->isReadLockAcquire(shared_call),
            api->isReadLockAcquire(&*shared_call));
  EXPECT_EQ(api->isWriteLockAcquire(shared_call),
            api->isWriteLockAcquire(&*shared_call));
  EXPECT_EQ(api->isReadLockAcquire(exclusive_call),
            api->isReadLockAcquire(&*exclusive_call));
  EXPECT_EQ(api->isWriteLockAcquire(exclusive_call),
            api->isWriteLockAcquire(&*exclusive_call));
}

TEST_F(ThreadAPITest, MapsOpenMPTaskRuntimeVariants) {
  const char *source = R"(
    declare i32 @__kmpc_omp_task_begin_if0(i8*, i32, i8*)
    declare i32 @__kmpc_omp_task_with_deps_51(i8*, i32, i8*, i32, i8*, i32, i8*, i8*)
    declare void @__kmpc_omp_task_complete_if0(i8*, i32, i8*)
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  EXPECT_EQ(api->getType(module->getFunction("__kmpc_omp_task_begin_if0")),
            ThreadAPI::TD_OMP_TASK);
  EXPECT_EQ(api->getType(module->getFunction("__kmpc_omp_task_with_deps_51")),
            ThreadAPI::TD_OMP_TASK_WITH_DEPS);
  EXPECT_EQ(api->getType(module->getFunction("__kmpc_omp_task_complete_if0")),
            ThreadAPI::TD_OMP_TASK_COMPLETE);
  EXPECT_EQ(api->getRuntimeLibrary(
                module->getFunction("__kmpc_omp_task_with_deps_51")),
            ThreadAPI::RuntimeLibrary::OpenMP);
  EXPECT_EQ(
      api->getSemanticTag(module->getFunction("__kmpc_omp_task_with_deps_51")),
      "task-with-deps");
}

TEST_F(ThreadAPITest, DistinguishesOpenMPDoacrossRuntimeVariants) {
  const char *source = R"(
    declare void @__kmpc_doacross_wait(i8*, i32, i64*)
    declare void @__kmpc_doacross_submit(i8*, i32, i64*)
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  EXPECT_EQ(api->getType(module->getFunction("__kmpc_doacross_wait")),
            ThreadAPI::TD_OMP_DOACROSS_WAIT);
  EXPECT_EQ(api->getType(module->getFunction("__kmpc_doacross_submit")),
            ThreadAPI::TD_OMP_DOACROSS_SUBMIT);
}

TEST_F(ThreadAPITest, MapsOpenMPRegionRuntimeVariants) {
  const char *source = R"(
    declare i32 @__kmpc_single(i8*, i32)
    declare void @__kmpc_end_single(i8*, i32)
    declare i32 @__kmpc_master(i8*, i32)
    declare void @__kmpc_end_master(i8*, i32)
    declare void @__kmpc_ordered(i8*, i32)
    declare void @__kmpc_end_ordered(i8*, i32)
    declare i32 @__kmpc_reduce(i8*, i32, i32, i64, i8*, void (i8*, i8*)*, [8 x i32]*)
    declare void @__kmpc_for_static_fini(i8*, i32)
    declare void @__kmpc_dispatch_fini_4(i8*, i32)
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  EXPECT_EQ(api->getType(module->getFunction("__kmpc_single")),
            ThreadAPI::TD_OMP_SINGLE_START);
  EXPECT_EQ(api->getType(module->getFunction("__kmpc_end_single")),
            ThreadAPI::TD_OMP_SINGLE_END);
  EXPECT_EQ(api->getType(module->getFunction("__kmpc_master")),
            ThreadAPI::TD_OMP_MASTER_START);
  EXPECT_EQ(api->getType(module->getFunction("__kmpc_end_master")),
            ThreadAPI::TD_OMP_MASTER_END);
  EXPECT_EQ(api->getType(module->getFunction("__kmpc_ordered")),
            ThreadAPI::TD_OMP_ORDERED_START);
  EXPECT_EQ(api->getType(module->getFunction("__kmpc_end_ordered")),
            ThreadAPI::TD_OMP_ORDERED_END);
  EXPECT_EQ(api->getType(module->getFunction("__kmpc_reduce")),
            ThreadAPI::TD_OMP_REDUCE_START);
  EXPECT_EQ(api->getType(module->getFunction("__kmpc_for_static_fini")),
            ThreadAPI::TD_OMP_FOR_STATIC_FINI);
  EXPECT_EQ(api->getType(module->getFunction("__kmpc_dispatch_fini_4")),
            ThreadAPI::TD_OMP_FOR_DISPATCH_FINI);
}

TEST_F(ThreadAPITest, DescribesMPIBarrierUsingStructuredConfig) {
  const char *source = R"(
    declare i32 @MPI_Ibarrier(i8*, i8*)
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  ThreadAPI::APIDescription desc =
      api->describe(module->getFunction("MPI_Ibarrier"));
  EXPECT_EQ(desc.type, ThreadAPI::TD_MPI_BARRIER);
  EXPECT_EQ(desc.library, ThreadAPI::RuntimeLibrary::MPI);
  EXPECT_EQ(desc.semantic_tag, "ibarrier");
  EXPECT_TRUE(desc.from_config);
}

TEST_F(ThreadAPITest, UsesCriticalNameAsAnalysisLockIdentity) {
  const char *source = R"(
    @crit = global [8 x i32] zeroinitializer

    declare void @__kmpc_critical(i8*, i32, [8 x i32]*)
    declare void @__kmpc_end_critical(i8*, i32, [8 x i32]*)

    define void @main() {
    entry:
      call void @__kmpc_critical(i8* null, i32 0, [8 x i32]* @crit)
      call void @__kmpc_end_critical(i8* null, i32 0, [8 x i32]* @crit)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  const Function *main_func = module->getFunction("main");
  ASSERT_NE(main_func, nullptr);

  auto it = main_func->getEntryBlock().begin();
  const Instruction *enter = &*it++;
  const Instruction *exit = &*it++;

  EXPECT_EQ(api->getAnalysisLockIdentity(enter),
            module->getNamedGlobal("crit"));
  EXPECT_EQ(api->getAnalysisLockIdentity(exit), module->getNamedGlobal("crit"));
}

TEST_F(ThreadAPITest, WrapperOperationsShareAnalysisLockIdentity) {
  const char *source = R"(
    @lock = global i8 0

    declare void @fake_unique_lockC1E(i8*, i8*)
    declare void @fake_unique_lockD1Ev(i8*)
    declare void @fake_unique_locklockEv(i8*)
    declare void @fake_unique_lockunlockEv(i8*)

    define void @main() {
    entry:
      %wrapper = alloca i8
      call void @fake_unique_lockC1E(i8* %wrapper, i8* @lock)
      call void @fake_unique_lockunlockEv(i8* %wrapper)
      call void @fake_unique_locklockEv(i8* %wrapper)
      call void @fake_unique_lockD1Ev(i8* %wrapper)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  const Function *main_func = module->getFunction("main");
  ASSERT_NE(main_func, nullptr);

  std::vector<const Instruction *> calls;
  for (const Instruction &inst : main_func->getEntryBlock()) {
    if (isa<CallBase>(&inst)) {
      calls.push_back(&inst);
    }
  }
  ASSERT_EQ(calls.size(), 4u);
  const Instruction *ctor = calls[0];
  const Instruction *unlock = calls[1];
  const Instruction *lock = calls[2];
  const Instruction *dtor = calls[3];

  const Value *identity = api->getAnalysisLockIdentity(ctor);
  ASSERT_NE(identity, nullptr);
  EXPECT_EQ(identity, api->getAnalysisLockIdentity(unlock));
  EXPECT_EQ(identity, api->getAnalysisLockIdentity(lock));
  EXPECT_EQ(identity, api->getAnalysisLockIdentity(dtor));
}
