#include "ThreadAPITestSupport.h"
#include "Concurrency/OpenMP/OpenMPModel.h"

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
TEST_F(ThreadAPITest, LockSemanticDescriptorCapturesModeAndOperation) {
  const char *source = R"(
    declare i32 @pthread_rwlock_rdlock(i8*)
    declare i32 @pthread_rwlock_wrlock(i8*)
    declare i32 @pthread_mutex_trylock(i8*)
    declare i32 @pthread_rwlock_unlock(i8*)

    define void @main(i8* %lock) {
    entry:
      call i32 @pthread_rwlock_rdlock(i8* %lock)
      call i32 @pthread_rwlock_wrlock(i8* %lock)
      call i32 @pthread_mutex_trylock(i8* %lock)
      call i32 @pthread_rwlock_unlock(i8* %lock)
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
  const Instruction *rdlock = &*it++;
  const Instruction *wrlock = &*it++;
  const Instruction *trylock = &*it++;
  const Instruction *unlock = &*it++;

  ThreadAPI::LockSemantics rd = api->describeLockSemantics(rdlock);
  ThreadAPI::LockSemantics wr = api->describeLockSemantics(wrlock);
  ThreadAPI::LockSemantics tr = api->describeLockSemantics(trylock);
  ThreadAPI::LockSemantics un = api->describeLockSemantics(unlock);

  EXPECT_TRUE(rd.is_lock_api);
  EXPECT_TRUE(rd.is_acquire);
  EXPECT_EQ(rd.mode, ThreadAPI::LockMode::Shared);

  EXPECT_TRUE(wr.is_lock_api);
  EXPECT_TRUE(wr.is_acquire);
  EXPECT_EQ(wr.mode, ThreadAPI::LockMode::Exclusive);

  EXPECT_TRUE(tr.is_lock_api);
  EXPECT_TRUE(tr.is_acquire);
  EXPECT_TRUE(tr.is_try);
  EXPECT_EQ(tr.mode, ThreadAPI::LockMode::Exclusive);

  EXPECT_TRUE(un.is_lock_api);
  EXPECT_TRUE(un.is_release);
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
TEST_F(ThreadAPITest,
       LongestPrefixRuleWinsForSpecializedOpenMPRuntimeFamilies) {
  const char *source = R"(
    declare void @__kmpc_teams_host(i8*, i32)
    declare void @__kmpc_teams_distribute_nowait_4(i8*, i32, i32*)
    declare void @__kmpc_distribute_static_init_4(i8*, i32, i32*)
    declare void @__kmpc_distribute_dynamic_init_4(i8*, i32, i32*)
    declare void @__kmpc_distribute_guidance_init_4(i8*, i32, i32*)
    declare void @__kmpc_loop_static_4(i8*, i32, i32*)
    declare void @__kmpc_loop_dynamic_4(i8*, i32, i32*)
    declare void @__kmpc_loop_guidance_4(i8*, i32, i32*)
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();

  EXPECT_EQ(api->getType(module->getFunction("__kmpc_teams_host")),
            ThreadAPI::TD_OMP_TEAMS_HOST);
  EXPECT_EQ(
      api->getType(module->getFunction("__kmpc_teams_distribute_nowait_4")),
      ThreadAPI::TD_OMP_TEAMS_DISTRIBUTE);
  EXPECT_EQ(
      api->getType(module->getFunction("__kmpc_distribute_static_init_4")),
      ThreadAPI::TD_OMP_DISTRIBUTE_STATIC);
  EXPECT_EQ(
      api->getType(module->getFunction("__kmpc_distribute_dynamic_init_4")),
      ThreadAPI::TD_OMP_DISTRIBUTE_DYNAMIC);
  EXPECT_EQ(
      api->getType(module->getFunction("__kmpc_distribute_guidance_init_4")),
      ThreadAPI::TD_OMP_DISTRIBUTE_GUIDANCE);
  EXPECT_EQ(api->getType(module->getFunction("__kmpc_loop_static_4")),
            ThreadAPI::TD_OMP_LOOP_STATIC_INIT);
  EXPECT_EQ(api->getType(module->getFunction("__kmpc_loop_dynamic_4")),
            ThreadAPI::TD_OMP_LOOP_DYNAMIC_INIT);
  EXPECT_EQ(api->getType(module->getFunction("__kmpc_loop_guidance_4")),
            ThreadAPI::TD_OMP_LOOP_GUIDANCE_INIT);
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
TEST_F(ThreadAPITest, ReportsSharedVsExclusiveLockSemantics) {
  const char *source = R"(
    @lock = global i8 0

    declare i32 @pthread_rwlock_rdlock(i8*)
    declare i32 @pthread_rwlock_wrlock(i8*)
    declare i32 @pthread_rwlock_unlock(i8*)

    define void @main() {
    entry:
      call i32 @pthread_rwlock_rdlock(i8* @lock)
      call i32 @pthread_rwlock_wrlock(i8* @lock)
      call i32 @pthread_rwlock_unlock(i8* @lock)
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
  ASSERT_EQ(calls.size(), 3u);

  const auto read_info = api->getLockSemanticInfo(calls[0]);
  const auto write_info = api->getLockSemanticInfo(calls[1]);
  const auto unlock_info = api->getLockSemanticInfo(calls[2]);

  EXPECT_TRUE(read_info.isShared());
  EXPECT_TRUE(write_info.isExclusive());
  EXPECT_TRUE(unlock_info.isRelease());
  EXPECT_EQ(read_info.identity, module->getNamedGlobal("lock"));
  EXPECT_EQ(write_info.identity, module->getNamedGlobal("lock"));
  EXPECT_EQ(unlock_info.identity, module->getNamedGlobal("lock"));
}
TEST_F(ThreadAPITest, ReportsExplicitSemanticLoweringStatus) {
  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();

  auto async = api->getSemanticLoweringInfo(ThreadAPI::TD_ASYNC);
  auto future_get = api->getSemanticLoweringInfo(ThreadAPI::TD_FUTURE_GET);
  auto omp_atomic =
      api->getSemanticLoweringInfo(ThreadAPI::TD_OMP_ATOMIC_START);
  auto task_complete =
      api->getSemanticLoweringInfo(ThreadAPI::TD_OMP_TASK_COMPLETE);
  auto doacross_submit =
      api->getSemanticLoweringInfo(ThreadAPI::TD_OMP_DOACROSS_SUBMIT);
  auto atomic_wait = api->getSemanticLoweringInfo(ThreadAPI::TD_ATOMIC_WAIT);

  EXPECT_EQ(async.kind, ThreadAPI::SemanticLoweringKind::Deferred);
  EXPECT_STREQ(async.reason, "async-launch-policy-witness");
  EXPECT_NE(async.owners, 0u);
  EXPECT_EQ(future_get.kind, ThreadAPI::SemanticLoweringKind::Modeled);
  EXPECT_STREQ(future_get.reason, "modeled");
  EXPECT_TRUE(api->hasSemanticLoweringOwner(
      ThreadAPI::TD_FUTURE_GET, ThreadAPI::SemanticLoweringOwner::HB));
  EXPECT_EQ(omp_atomic.kind,
            ThreadAPI::SemanticLoweringKind::RecognizedButUnmodeled);
  EXPECT_STREQ(omp_atomic.reason, "openmp-atomic-runtime-unmodeled");
  EXPECT_TRUE(api->hasSemanticLoweringOwner(
      ThreadAPI::TD_OMP_ATOMIC_START,
      ThreadAPI::SemanticLoweringOwner::ExplicitFallback));
  EXPECT_EQ(task_complete.kind, ThreadAPI::SemanticLoweringKind::Modeled);
  EXPECT_STREQ(task_complete.reason, "modeled");
  EXPECT_TRUE(
      api->hasSemanticLoweringOwner(ThreadAPI::TD_OMP_TASK_COMPLETE,
                                    ThreadAPI::SemanticLoweringOwner::OpenMP));
  EXPECT_EQ(doacross_submit.kind, ThreadAPI::SemanticLoweringKind::Modeled);
  EXPECT_STREQ(doacross_submit.reason, "modeled");
  EXPECT_TRUE(api->hasSemanticLoweringOwner(
      ThreadAPI::TD_OMP_DOACROSS_SUBMIT, ThreadAPI::SemanticLoweringOwner::HB));
  EXPECT_EQ(atomic_wait.kind,
            ThreadAPI::SemanticLoweringKind::RecognizedButUnmodeled);
  EXPECT_STREQ(atomic_wait.reason, "cpp-atomic-wait-runtime-unmodeled");
}
TEST_F(ThreadAPITest, OpenMPBarrierUsesSiteIdentityInsteadOfMetadataOperand) {
  const char *source = R"(
    declare void @__kmpc_barrier(i8*, i32)

    define void @main() {
    entry:
      call void @__kmpc_barrier(i8* null, i32 0)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  const Function *main_func = module->getFunction("main");
  ASSERT_NE(main_func, nullptr);
  const Instruction *barrier = &main_func->getEntryBlock().front();

  EXPECT_EQ(api->getType(module->getFunction("__kmpc_barrier")),
            ThreadAPI::TD_BAR_WAIT);
  EXPECT_EQ(api->getBarrierVal(barrier), barrier);
}
TEST_F(ThreadAPITest, SpecialSemanticLoweringStatesStayExplicitlyEnumerated) {
  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();

  std::set<ThreadAPI::TD_TYPE> expected_non_modeled = {
      ThreadAPI::TD_DUMMY,
      ThreadAPI::TD_ASYNC,
      ThreadAPI::TD_CUDA_MULTI_DEVICE_LAUNCH,
      ThreadAPI::TD_OMP_ATOMIC_START,
      ThreadAPI::TD_OMP_ATOMIC_END,
      ThreadAPI::TD_OMP_CANCEL,
      ThreadAPI::TD_OMP_TARGET_DATA_UPDATE,
      ThreadAPI::TD_OMP_TEAMS,
      ThreadAPI::TD_OMP_TEAMS_HOST,
      ThreadAPI::TD_OMP_TEAMS_DISTRIBUTE,
      ThreadAPI::TD_OMP_DISTRIBUTE,
      ThreadAPI::TD_OMP_DISTRIBUTE_STATIC,
      ThreadAPI::TD_OMP_DISTRIBUTE_DYNAMIC,
      ThreadAPI::TD_OMP_DISTRIBUTE_GUIDANCE,
      ThreadAPI::TD_OMP_LOOP_STATIC_INIT,
      ThreadAPI::TD_OMP_LOOP_DYNAMIC_INIT,
      ThreadAPI::TD_OMP_LOOP_GUIDANCE_INIT,
      ThreadAPI::TD_OMP_AFFINITY,
      ThreadAPI::TD_OMP_SCOPE_START,
      ThreadAPI::TD_OMP_SCOPE_END,
      ThreadAPI::TD_OMP_TASKLOOP_SIMD,
      ThreadAPI::TD_OMP_TASKLOOP_FINI,
      ThreadAPI::TD_OMP_INTEROP_INIT,
      ThreadAPI::TD_OMP_INTEROP_FINI,
      ThreadAPI::TD_SEMAPHORE_ACQUIRE,
      ThreadAPI::TD_SEMAPHORE_RELEASE,
      ThreadAPI::TD_SEMAPHORE_TRY_ACQUIRE,
      ThreadAPI::TD_ATOMIC_WAIT,
      ThreadAPI::TD_ATOMIC_NOTIFY_ONE,
      ThreadAPI::TD_ATOMIC_NOTIFY_ALL,
      ThreadAPI::TD_JTHREAD_DTOR,
      ThreadAPI::TD_MPI_SESSION_GET_INFO,
      ThreadAPI::TD_MPI_SESSION_GET_NUM_ERRCODES,
      ThreadAPI::TD_MPI_SESSION_GET_ERRHANDLER,
      ThreadAPI::TD_MPI_SESSION_SET_ERRHANDLER,
      ThreadAPI::TD_MPI_ERRHANDLER_CREATE,
      ThreadAPI::TD_MPI_ERRHANDLER_FREE,
      ThreadAPI::TD_MPI_COMM_GET_ERRHANDLER,
      ThreadAPI::TD_MPI_COMM_SET_ERRHANDLER,
      ThreadAPI::TD_MPI_COMM_CALL_ERRHANDLER,
      ThreadAPI::TD_MPI_WIN_GET_ERRHANDLER,
      ThreadAPI::TD_MPI_WIN_SET_ERRHANDLER,
      ThreadAPI::TD_MPI_FILE_GET_ERRHANDLER,
      ThreadAPI::TD_MPI_FILE_SET_ERRHANDLER,
      ThreadAPI::TD_MPI_ERROR_CLASS,
      ThreadAPI::TD_MPI_ERROR_STRING,
      ThreadAPI::TD_MPI_INFO_CREATE,
      ThreadAPI::TD_MPI_INFO_DUP,
      ThreadAPI::TD_MPI_INFO_FREE,
      ThreadAPI::TD_MPI_INFO_GET,
      ThreadAPI::TD_MPI_INFO_GET_VALUELEN,
      ThreadAPI::TD_MPI_INFO_GET_NKEYS,
      ThreadAPI::TD_MPI_INFO_GET_NTHKEY,
      ThreadAPI::TD_MPI_INFO_GET_KEYVAL,
      ThreadAPI::TD_MPI_INFO_SET,
      ThreadAPI::TD_MPI_INFO_DELETE,
      ThreadAPI::TD_MPI_INFO_C2F,
      ThreadAPI::TD_MPI_INFO_CREATE_ENV,
      ThreadAPI::TD_MPI_INFO_FREE_ENV,
      ThreadAPI::TD_MPI_GET_COUNT,
      ThreadAPI::TD_MPI_GET_ELEMENTS,
      ThreadAPI::TD_MPI_GET_ELEMENTS_X,
      ThreadAPI::TD_MPI_STATUS_SIZE,
      ThreadAPI::TD_MPI_STATUS_SET_ELEMENTS,
      ThreadAPI::TD_MPI_STATUS_SET_ELEMENTS_X,
      ThreadAPI::TD_KERNEL_ATOMIC_READ,
      ThreadAPI::TD_KERNEL_ATOMIC_WRITE,
      ThreadAPI::TD_KERNEL_ATOMIC_RMW,
  };

  for (int raw = static_cast<int>(ThreadAPI::TD_DUMMY);
       raw <= static_cast<int>(ThreadAPI::TD_KERNEL_MEMORY_BARRIER); ++raw) {
    ThreadAPI::TD_TYPE type = static_cast<ThreadAPI::TD_TYPE>(raw);
    const char *name = ThreadAPI::tdTypeToString(type);
    ASSERT_NE(name, nullptr);
    ASSERT_NE(name[0], '\0');

    ThreadAPI::SemanticLoweringInfo info = api->getSemanticLoweringInfo(type);
    ASSERT_NE(info.reason, nullptr);
    EXPECT_NE(info.reason[0], '\0') << name;

    if (expected_non_modeled.count(type) != 0) {
      EXPECT_NE(info.kind, ThreadAPI::SemanticLoweringKind::Modeled) << name;
      if (type != ThreadAPI::TD_ASYNC &&
          info.kind != ThreadAPI::SemanticLoweringKind::Deferred) {
        EXPECT_NE(info.owners &
                      ThreadAPI::semanticLoweringOwnerMask(
                          ThreadAPI::SemanticLoweringOwner::ExplicitFallback),
                  0u)
            << name;
      }
    } else {
      EXPECT_EQ(info.kind, ThreadAPI::SemanticLoweringKind::Modeled) << name;
      EXPECT_NE(info.owners, 0u) << name;
    }
  }
}
TEST_F(ThreadAPITest, ModeledConcurrencyFunctionsExposeConcreteLoweringOwners) {
  const char *source = R"(
    declare void @__kmpc_critical(i8*, i32, [8 x i32]*)
    declare i32 @MPI_Comm_idup(i8*, i8*, i8*)

    @crit = global [8 x i32] zeroinitializer

    define void @main(i8* %comm, i8* %newcomm, i8* %req) {
    entry:
      call void @__kmpc_critical(i8* null, i32 0, [8 x i32]* @crit)
      call i32 @MPI_Comm_idup(i8* %comm, i8* %newcomm, i8* %req)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  const Function *critical = module->getFunction("__kmpc_critical");
  const Function *idup = module->getFunction("MPI_Comm_idup");
  ASSERT_NE(critical, nullptr);
  ASSERT_NE(idup, nullptr);

  ThreadAPI::SemanticLoweringInfo critical_info =
      api->getSemanticLoweringInfo(critical);
  EXPECT_EQ(critical_info.kind, ThreadAPI::SemanticLoweringKind::Modeled);
  EXPECT_TRUE(api->hasSemanticLoweringOwner(
      critical, ThreadAPI::SemanticLoweringOwner::OpenMP));
  EXPECT_TRUE(api->hasSemanticLoweringOwner(
      critical, ThreadAPI::SemanticLoweringOwner::LockSet));

  ThreadAPI::SemanticLoweringInfo idup_info =
      api->getSemanticLoweringInfo(idup);
  EXPECT_EQ(idup_info.kind, ThreadAPI::SemanticLoweringKind::Modeled);
  EXPECT_TRUE(api->hasSemanticLoweringOwner(
      idup, ThreadAPI::SemanticLoweringOwner::MPI));
}
TEST_F(ThreadAPITest, LongestPrefixRuleWinsForOpenMPDoacross) {
  const char *source = R"(
    declare void @__kmpc_doacross_wait_4(i8*, i32, i64*)
    declare void @__kmpc_doacross_submit_4(i8*, i32, i64*)
    declare void @__kmpc_doacross_init_4(i8*, i32, i64*)
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  EXPECT_EQ(api->getType(module->getFunction("__kmpc_doacross_wait_4")),
            ThreadAPI::TD_OMP_DOACROSS_WAIT);
  EXPECT_EQ(api->getType(module->getFunction("__kmpc_doacross_submit_4")),
            ThreadAPI::TD_OMP_DOACROSS_SUBMIT);
  EXPECT_EQ(api->getType(module->getFunction("__kmpc_doacross_init_4")),
            ThreadAPI::TD_OMP_DOACROSS_INIT);
}
TEST_F(ThreadAPITest, SemaphoreLoweringIsExplicitForBinaryAndCountingForms) {
  const char *source = R"(
    declare i32 @binary_sem_wait(i8*)
    declare void @_ZNSt16binary_semaphore7acquireEv(i8*)
    declare void @_ZNSt18counting_semaphore7acquireEv(i8*)

    define void @main(i8* %sem) {
    entry:
      call i32 @binary_sem_wait(i8* %sem)
      call void @_ZNSt16binary_semaphore7acquireEv(i8* %sem)
      call void @_ZNSt18counting_semaphore7acquireEv(i8* %sem)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();

  const Function *binary_sem_wait = module->getFunction("binary_sem_wait");
  const Function *binary_cpp =
      module->getFunction("_ZNSt16binary_semaphore7acquireEv");
  const Function *counting_cpp =
      module->getFunction("_ZNSt18counting_semaphore7acquireEv");
  ASSERT_NE(binary_sem_wait, nullptr);
  ASSERT_NE(binary_cpp, nullptr);
  ASSERT_NE(counting_cpp, nullptr);

  auto binary_info = api->getSemanticLoweringInfo(binary_sem_wait);
  EXPECT_EQ(binary_info.kind, ThreadAPI::SemanticLoweringKind::Modeled);
  EXPECT_TRUE(api->hasSemanticLoweringOwner(
      binary_sem_wait, ThreadAPI::SemanticLoweringOwner::LockSet));

  auto binary_cpp_info = api->getSemanticLoweringInfo(binary_cpp);
  EXPECT_EQ(binary_cpp_info.kind, ThreadAPI::SemanticLoweringKind::Modeled);
  EXPECT_TRUE(api->hasSemanticLoweringOwner(
      binary_cpp, ThreadAPI::SemanticLoweringOwner::LockSet));

  auto counting_info = api->getSemanticLoweringInfo(counting_cpp);
  EXPECT_EQ(counting_info.kind,
            ThreadAPI::SemanticLoweringKind::RecognizedButUnmodeled);
  EXPECT_STREQ(counting_info.reason, "counting-semaphore-runtime-unmodeled");
  EXPECT_TRUE(api->hasSemanticLoweringOwner(
      counting_cpp, ThreadAPI::SemanticLoweringOwner::ExplicitFallback));

  auto generic_info =
      api->getSemanticLoweringInfo(ThreadAPI::TD_SEMAPHORE_ACQUIRE);
  EXPECT_EQ(generic_info.kind,
            ThreadAPI::SemanticLoweringKind::RecognizedButUnmodeled);
}
TEST_F(ThreadAPITest, RecognizesCppAtomicWaitNotifyAndJthreadDestructor) {
  const char *source = R"(
    declare void @_ZNSt6atomicIiE4waitEi(i8*, i32)
    declare void @_ZNSt6atomicIiE10notify_oneEv(i8*)
    declare void @_ZNSt6atomicIiE10notify_allEv(i8*)
    declare void @_ZNSt7jthreadD1Ev(i8*)

    define void @main() {
    entry:
      %thr = alloca i8
      call void @_ZNSt7jthreadD1Ev(i8* %thr)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  const Function *atomic_wait = module->getFunction("_ZNSt6atomicIiE4waitEi");
  const Function *notify_one =
      module->getFunction("_ZNSt6atomicIiE10notify_oneEv");
  const Function *notify_all =
      module->getFunction("_ZNSt6atomicIiE10notify_allEv");
  const Function *jthread_dtor = module->getFunction("_ZNSt7jthreadD1Ev");
  ASSERT_NE(atomic_wait, nullptr);
  ASSERT_NE(notify_one, nullptr);
  ASSERT_NE(notify_all, nullptr);
  ASSERT_NE(jthread_dtor, nullptr);

  EXPECT_EQ(api->getType(atomic_wait), ThreadAPI::TD_ATOMIC_WAIT);
  EXPECT_EQ(api->getType(notify_one), ThreadAPI::TD_ATOMIC_NOTIFY_ONE);
  EXPECT_EQ(api->getType(notify_all), ThreadAPI::TD_ATOMIC_NOTIFY_ALL);
  EXPECT_EQ(api->getType(jthread_dtor), ThreadAPI::TD_JTHREAD_DTOR);
  EXPECT_EQ(api->getSemanticLoweringInfo(atomic_wait).kind,
            ThreadAPI::SemanticLoweringKind::RecognizedButUnmodeled);
  EXPECT_EQ(api->getSemanticLoweringInfo(jthread_dtor).kind,
            ThreadAPI::SemanticLoweringKind::RecognizedButUnmodeled);
  EXPECT_STREQ(api->getSemanticLoweringInfo(jthread_dtor).reason,
               "jthread-autojoin-lifetime-unmodeled");

  const Function *main_func = module->getFunction("main");
  ASSERT_NE(main_func, nullptr);
  const Instruction *call = &*main_func->getEntryBlock().begin();
  EXPECT_FALSE(api->isTDJoin(call));
}
TEST_F(ThreadAPITest, CppWrapperLockIdentityResolvesUnderlyingMutex) {
  const char *source = R"(
    declare void @fake_unique_lockC1E(i8*, i8*)
    declare void @fake_unique_lockD1Ev(i8*)

    @lock = global i8 0

    define void @main() {
    entry:
      %wrapper = alloca i8
      call void @fake_unique_lockC1E(i8* %wrapper, i8* @lock)
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

  auto it = main_func->getEntryBlock().begin();
  ++it;
  const Instruction *ctor = &*it++;
  const Instruction *dtor = &*it++;
  const Value *lock = module->getNamedGlobal("lock");
  ASSERT_NE(lock, nullptr);

  EXPECT_EQ(api->getAnalysisLockIdentity(ctor), lock);
  EXPECT_EQ(api->getAnalysisLockIdentity(dtor), lock);
}
TEST_F(ThreadAPITest, MPIConfiguredAPIsHaveConsistentLoweringLibraries) {
  const char *source = R"(
    declare i32 @MPI_Session_get_info(i8*, i8*)
    declare i32 @MPI_Type_get_extent(i32, i64*, i64*)
    declare i32 @MPI_Cart_create(i8*, i32, i32*, i32*, i32, i8*)

    define i32 @main(i8* %session, i8* %info, i64* %lb, i64* %extent,
                     i8* %comm, i32* %dims, i32* %periods, i8* %newcomm) {
    entry:
      call i32 @MPI_Session_get_info(i8* %session, i8* %info)
      call i32 @MPI_Type_get_extent(i32 0, i64* %lb, i64* %extent)
      call i32 @MPI_Cart_create(i8* %comm, i32 1, i32* %dims, i32* %periods,
                                i32 0, i8* %newcomm)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();

  const Function *session = module->getFunction("MPI_Session_get_info");
  const Function *extent = module->getFunction("MPI_Type_get_extent");
  const Function *cart = module->getFunction("MPI_Cart_create");
  ASSERT_NE(session, nullptr);
  ASSERT_NE(extent, nullptr);
  ASSERT_NE(cart, nullptr);

  EXPECT_NE(api->getType(session), ThreadAPI::TD_DUMMY);
  EXPECT_NE(api->getType(extent), ThreadAPI::TD_DUMMY);
  EXPECT_NE(api->getType(cart), ThreadAPI::TD_DUMMY);

  auto session_type_info = api->getSemanticLoweringInfo(api->getType(session));
  auto session_func_info = api->getSemanticLoweringInfo(session);
  EXPECT_EQ(session_type_info.kind, session_func_info.kind);
  EXPECT_EQ(session_type_info.owners, session_func_info.owners);

  auto extent_type_info = api->getSemanticLoweringInfo(api->getType(extent));
  auto extent_func_info = api->getSemanticLoweringInfo(extent);
  EXPECT_EQ(extent_type_info.kind, extent_func_info.kind);
  EXPECT_EQ(extent_type_info.owners, extent_func_info.owners);

  auto cart_type_info = api->getSemanticLoweringInfo(api->getType(cart));
  auto cart_func_info = api->getSemanticLoweringInfo(cart);
  EXPECT_EQ(cart_type_info.kind, cart_func_info.kind);
  EXPECT_EQ(cart_type_info.owners, cart_func_info.owners);
}
TEST_F(ThreadAPITest, NormalizesWrappedAndOpenMPIForwarderNames) {
  const char *source = R"(
    declare i32 @__wrap_MPI_Barrier(i8*)
    declare i32 @__wrap_PMPI_Bcast(i8*, i32, i32, i32, i8*)
    declare i32 @ompi_mpi_allreduce(i8*, i8*, i32, i32, i32, i8*)

    define i32 @main(i8* %comm) {
    entry:
      call i32 @__wrap_MPI_Barrier(i8* %comm)
      call i32 @__wrap_PMPI_Bcast(i8* null, i32 1, i32 0, i32 0, i8* %comm)
      call i32 @ompi_mpi_allreduce(i8* null, i8* null, i32 1, i32 0, i32 0,
                                   i8* %comm)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  EXPECT_EQ(api->getType(module->getFunction("__wrap_MPI_Barrier")),
            ThreadAPI::TD_MPI_BARRIER);
  EXPECT_EQ(api->getType(module->getFunction("__wrap_PMPI_Bcast")),
            ThreadAPI::TD_MPI_BCAST);
  EXPECT_EQ(api->getType(module->getFunction("ompi_mpi_allreduce")),
            ThreadAPI::TD_MPI_ALLREDUCE);
  EXPECT_EQ(api->getRuntimeLibrary(module->getFunction("ompi_mpi_allreduce")),
            ThreadAPI::RuntimeLibrary::MPI);
}
TEST_F(ThreadAPITest, RecognizesGOMPTaskAndBarrierRuntimeAliases) {
  const char *source = R"(
    declare void @GOMP_barrier()
    declare void @GOMP_taskwait()
    declare void @GOMP_taskgroup_start()
    declare void @GOMP_taskgroup_end()
    declare void @GOMP_task(void ()*, i8*, i8*, i64, i64, i1, i32, i8*, i32)

    define void @worker() {
    entry:
      ret void
    }

    define void @main() {
    entry:
      call void @GOMP_task(void ()* @worker, i8* null, i8* null, i64 0, i64 0,
                           i1 true, i32 0, i8* null, i32 0)
      call void @GOMP_taskwait()
      call void @GOMP_taskgroup_start()
      call void @GOMP_taskgroup_end()
      call void @GOMP_barrier()
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  EXPECT_EQ(api->getType(module->getFunction("GOMP_task")),
            ThreadAPI::TD_OMP_TASK);
  EXPECT_EQ(api->getType(module->getFunction("GOMP_taskwait")),
            ThreadAPI::TD_OMP_TASKWAIT);
  EXPECT_EQ(api->getType(module->getFunction("GOMP_taskgroup_start")),
            ThreadAPI::TD_OMP_TASKGROUP_START);
  EXPECT_EQ(api->getType(module->getFunction("GOMP_taskgroup_end")),
            ThreadAPI::TD_OMP_TASKGROUP_END);
  EXPECT_EQ(api->getType(module->getFunction("GOMP_barrier")),
            ThreadAPI::TD_BAR_WAIT);
}
TEST_F(ThreadAPITest, RecognizesCriticalWithHintAsCriticalEntry) {
  const char *source = R"(
    declare void @__kmpc_critical_with_hint(i8*, i32, i8*, i64)

    define void @main(i8* %lock) {
    entry:
      call void @__kmpc_critical_with_hint(i8* null, i32 0, i8* %lock, i64 1)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  EXPECT_EQ(api->getType(module->getFunction("__kmpc_critical_with_hint")),
            ThreadAPI::TD_ACQUIRE);
  const Function *main_func = module->getFunction("main");
  ASSERT_NE(main_func, nullptr);
  const Instruction *call = &main_func->getEntryBlock().front();
  EXPECT_EQ(api->getAnalysisLockIdentity(call),
            cast<CallBase>(call)->getArgOperand(2));
}

TEST_F(ThreadAPITest, TaggedRAIIConstructorsExposeOwnershipSemantics) {
  const char *source = R"(
    declare void @fake_unique_lock_defer_lock_C1E(i8*, i8*, i8*)
    declare void @fake_unique_lock_try_to_lock_C1E(i8*, i8*, i8*)
    declare void @fake_unique_lock_adopt_lock_C1E(i8*, i8*, i8*)
    declare void @fake_unique_lock_try_lockEv(i8*)
    declare void @fake_unique_lock_D1Ev(i8*)

    @lock = global i8 0
    @tag = global i8 0

    define void @main() {
    entry:
      %deferred = alloca i8
      %tried = alloca i8
      %adopted = alloca i8
      call void @fake_unique_lock_defer_lock_C1E(i8* %deferred, i8* @lock, i8* @tag)
      call void @fake_unique_lock_D1Ev(i8* %deferred)
      call void @fake_unique_lock_try_to_lock_C1E(i8* %tried, i8* @lock, i8* @tag)
      call void @fake_unique_lock_D1Ev(i8* %tried)
      call void @fake_unique_lock_adopt_lock_C1E(i8* %adopted, i8* @lock, i8* @tag)
      call void @fake_unique_lock_try_lockEv(i8* %deferred)
      call void @fake_unique_lock_D1Ev(i8* %adopted)
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
  for (const Instruction &inst : instructions(*main_func))
    if (isa<CallBase>(&inst))
      calls.push_back(&inst);
  ASSERT_EQ(calls.size(), 7u);

  auto deferred = api->getLockSemanticInfo(calls[0]);
  EXPECT_EQ(deferred.ownership, ThreadAPI::LockOwnershipEffect::Deferred);
  EXPECT_FALSE(api->isTDAcquire(calls[0]));
  EXPECT_TRUE(api->getLockSemanticInfo(calls[1]).conditional);

  auto tried = api->getLockSemanticInfo(calls[2]);
  EXPECT_EQ(tried.ownership, ThreadAPI::LockOwnershipEffect::Try);
  EXPECT_TRUE(tried.is_try);
  EXPECT_TRUE(tried.conditional);
  EXPECT_TRUE(api->getLockSemanticInfo(calls[3]).conditional);

  auto adopted = api->getLockSemanticInfo(calls[4]);
  EXPECT_EQ(adopted.ownership, ThreadAPI::LockOwnershipEffect::Adopt);
  EXPECT_FALSE(api->isTDAcquire(calls[4]));
  EXPECT_TRUE(api->isTryLock(calls[5]));
  EXPECT_TRUE(api->isTDRelease(calls[6]));
}

TEST_F(ThreadAPITest, ScopedLockExposesEveryUnderlyingMutex) {
  const char *source = R"(
    declare void @fake_scoped_lock_C1E(i8*, i8*, i8*)
    declare void @fake_scoped_lock_D1Ev(i8*)
    @lock1 = global i8 0
    @lock2 = global i8 0
    define void @main() {
    entry:
      %wrapper = alloca i8
      call void @fake_scoped_lock_C1E(i8* %wrapper, i8* @lock1, i8* @lock2)
      call void @fake_scoped_lock_D1Ev(i8* %wrapper)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  const Function *main_func = module->getFunction("main");
  auto it = main_func->getEntryBlock().begin();
  ++it;
  const Instruction *ctor = &*it++;
  const Instruction *dtor = &*it;

  for (const Instruction *inst : {ctor, dtor}) {
    auto identities = api->getAnalysisLockIdentities(inst);
    ASSERT_EQ(identities.size(), 2u);
    EXPECT_EQ(identities[0], module->getNamedGlobal("lock1"));
    EXPECT_EQ(identities[1], module->getNamedGlobal("lock2"));
    EXPECT_EQ(api->getAnalysisLockIdentity(inst), nullptr);
  }
}

TEST_F(ThreadAPITest, ClassifiesTryLockModesWithoutBlockingShadowing) {
  const char *source = R"(
    declare i32 @_ZNSt5mutex8try_lockEv(i8*)
    declare i32 @_ZNSt12shared_mutex15try_lock_sharedEv(i8*)
    declare i32 @_ZNSt12shared_mutex8try_lockEv(i8*)
    declare i32 @_ZNSt18shared_timed_mutex19try_lock_shared_forEi(i8*, i32)
    declare i32 @_ZNSt18shared_timed_mutex12try_lock_forEi(i8*, i32)
    declare i32 @_ZNSt20counting_semaphoreILl4EE11try_acquireEv(i8*)
    define void @main(i8* %lock) {
      call i32 @_ZNSt5mutex8try_lockEv(i8* %lock)
      call i32 @_ZNSt12shared_mutex15try_lock_sharedEv(i8* %lock)
      call i32 @_ZNSt12shared_mutex8try_lockEv(i8* %lock)
      call i32 @_ZNSt18shared_timed_mutex19try_lock_shared_forEi(i8* %lock,
                                                                 i32 1)
      call i32 @_ZNSt18shared_timed_mutex12try_lock_forEi(i8* %lock, i32 1)
      call i32 @_ZNSt20counting_semaphoreILl4EE11try_acquireEv(i8* %lock)
      ret void
    }
  )";
  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();

  const std::vector<ThreadAPI::LockMode> expected_modes = {
      ThreadAPI::LockMode::Exclusive, ThreadAPI::LockMode::Shared,
      ThreadAPI::LockMode::Exclusive, ThreadAPI::LockMode::Shared,
      ThreadAPI::LockMode::Exclusive};
  auto it = module->getFunction("main")->front().begin();
  for (ThreadAPI::LockMode expected_mode : expected_modes) {
    const Instruction *call = &*it++;
    const auto semantics = api->describeLockSemantics(call);
    EXPECT_TRUE(semantics.is_acquire);
    EXPECT_TRUE(semantics.is_try);
    EXPECT_EQ(semantics.mode, expected_mode);
  }
  const Instruction *semaphore_try = &*it;
  EXPECT_EQ(api->getType(api->getCallee(semaphore_try)),
            ThreadAPI::TD_SEMAPHORE_TRY_ACQUIRE);
  EXPECT_TRUE(api->isTryLock(semaphore_try));
}

TEST_F(ThreadAPITest, OpenMPForkCarriesAllVariadicCaptures) {
  const char *source = R"(
    declare void @__kmpc_fork_call(i8*, i32, void (i32*, i32*, ...)*, ...)
    define internal void @outlined(i32* %gtid, i32* %btid, ...) { ret void }
    define void @main(i8* %a, i8* %b) {
      call void (i8*, i32, void (i32*, i32*, ...)*, ...)
        @__kmpc_fork_call(i8* null, i32 2,
                          void (i32*, i32*, ...)* @outlined, i8* %a, i8* %b)
      ret void
    }
  )";
  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  const Instruction *fork = &module->getFunction("main")->front().front();
  EXPECT_EQ(api->getForkedThread(fork), nullptr);
  EXPECT_EQ(api->getForkedFun(fork), module->getFunction("outlined"));
  auto payload = api->getForkPayloadArgs(fork);
  ASSERT_EQ(payload.size(), 2u);
  EXPECT_EQ(payload[0], module->getFunction("main")->getArg(0));
  EXPECT_EQ(payload[1], module->getFunction("main")->getArg(1));
}

TEST_F(ThreadAPITest, JoinAndHareAccessorsAreBoundsChecked) {
  const char *source = R"(
    declare void @_ZNSt6thread4joinEv(i8*)
    declare void @_ZNSt7jthread4joinEv(i8*)
    declare void @hare_parallel_for(i8*)
    define void @main(i8* %handle) {
      call void @_ZNSt6thread4joinEv(i8* %handle)
      call void @_ZNSt7jthread4joinEv(i8* %handle)
      call void @hare_parallel_for(i8* %handle)
      ret void
    }
  )";
  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  auto it = module->getFunction("main")->front().begin();
  const Instruction *join = &*it++;
  const Instruction *jjoin = &*it++;
  const Instruction *hare = &*it++;
  EXPECT_NE(api->getJoinedThread(join), nullptr);
  EXPECT_EQ(api->getRetParmAtJoinedSite(join), nullptr);
  EXPECT_NE(api->getJoinedThread(jjoin), nullptr);
  EXPECT_EQ(api->getRetParmAtJoinedSite(jjoin), nullptr);
  EXPECT_EQ(api->getTaskFuncAtHareParForSite(hare), nullptr);
  EXPECT_EQ(api->getTaskDataAtHareParForSite(hare), nullptr);
}

TEST_F(ThreadAPITest, UnwrapsOrdinaryConditionVariableWaitMutex) {
  const char *source = R"(
    declare void @fake_unique_lockC1E(i8*, i8*)
    declare void @_ZNSt18condition_variable4waitERSt11unique_lockISt5mutexE(
        i8*, i8*)
    @cv = global i8 0
    @mutex = global i8 0
    define void @main() {
      %wrapper = alloca i8
      call void @fake_unique_lockC1E(i8* %wrapper, i8* @mutex)
      call void @_ZNSt18condition_variable4waitERSt11unique_lockISt5mutexE(
          i8* @cv, i8* %wrapper)
      ret void
    }
  )";
  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  auto it = module->getFunction("main")->front().begin();
  ++it;
  const Instruction *ctor = &*it++;
  const Instruction *wait = &*it;
  EXPECT_EQ(api->getCondMutex(wait), module->getNamedGlobal("mutex"));
  EXPECT_EQ(api->getCondMutex(wait), api->getAnalysisLockIdentity(ctor));
}

TEST_F(ThreadAPITest, SeparatesDirectCUDALaunchFromGraphMutation) {
  const char *source = R"(
    declare i32 @cudaLaunchKernel(i8*, i8*, i8*, i8**, i64, i8*)
    declare i32 @cudaGraphAddKernelNode(i8*, i8*, i8*, i64, i8*)
    declare void @cleanup()
    define void @kernel() { ret void }
    define void @main(i8** %args) {
      call i32 @cudaLaunchKernel(i8* bitcast (void ()* @kernel to i8*),
                                 i8* null, i8* null, i8** %args, i64 0,
                                 i8* null)
      call void @cleanup()
      call i32 @cudaGraphAddKernelNode(i8* null, i8* null, i8* null,
                                       i64 0, i8* null)
      call void @cleanup()
      ret void
    }
  )";
  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  auto it = module->getFunction("main")->front().begin();
  const Instruction *launch = &*it++;
  ++it;
  const Instruction *graph_add = &*it;
  EXPECT_TRUE(api->isForkLike(launch));
  EXPECT_EQ(api->getCUDALaunchedKernel(launch), module->getFunction("kernel"));
  EXPECT_NE(api->getCUDALaunchedKernel(launch), module->getFunction("cleanup"));
  auto payload = api->getForkPayloadArgs(launch);
  ASSERT_EQ(payload.size(), 1u);
  EXPECT_EQ(payload[0], module->getFunction("main")->getArg(0));
  EXPECT_EQ(api->getType(api->getCallee(graph_add)), ThreadAPI::TD_CUDA_STREAM);
  EXPECT_FALSE(api->isForkLike(graph_add));
  EXPECT_EQ(api->getCUDALaunchedKernel(graph_add), nullptr);
}

TEST_F(ThreadAPITest, ResolvesAliasesAndConfiguredLinkerWrappers) {
  const char *source = R"(
    declare i32 @pthread_mutex_lock(i8*)
    @pthread_mutex_lock_alias = alias i32 (i8*),
        i32 (i8*)* @pthread_mutex_lock
    declare i32 @__wrap_pthread_mutex_lock(i8*)
    define void @main(i8* %mutex) {
      call i32 @pthread_mutex_lock_alias(i8* %mutex)
      call i32 @__wrap_pthread_mutex_lock(i8* %mutex)
      ret void
    }
  )";
  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  auto it = module->getFunction("main")->front().begin();
  const Instruction *alias_call = &*it++;
  const Instruction *wrapper_call = &*it;
  EXPECT_EQ(api->getCallee(alias_call), module->getFunction("pthread_mutex_lock"));
  EXPECT_EQ(api->getType(api->getCallee(alias_call)), ThreadAPI::TD_ACQUIRE);
  EXPECT_EQ(api->getType(api->getCallee(wrapper_call)), ThreadAPI::TD_ACQUIRE);
  EXPECT_EQ(api->getAnalysisLockIdentity(alias_call),
            module->getFunction("main")->getArg(0));
  EXPECT_EQ(api->getAnalysisLockIdentity(wrapper_call),
            module->getFunction("main")->getArg(0));
}

TEST_F(ThreadAPITest, PreservesLinuxTryModesAndRecognizesAtomics) {
  const char *source = R"(
    declare i32 @pthread_rwlock_tryrdlock(i8*)
    declare i32 @pthread_rwlock_trywrlock(i8*)
    declare i32 @down_trylock(i8*)
    declare i32 @down_read_trylock(i8*)
    declare i32 @down_write_trylock(i8*)
    declare i32 @atomic_read(i8*)
    declare void @atomic_set(i8*, i32)
    declare i32 @atomic_cmpxchg(i8*, i32, i32)
    declare i32 @test_and_set_bit(i32, i8*)
    define void @main(i8* %lock) {
      call i32 @pthread_rwlock_tryrdlock(i8* %lock)
      call i32 @pthread_rwlock_trywrlock(i8* %lock)
      call i32 @down_trylock(i8* %lock)
      call i32 @down_read_trylock(i8* %lock)
      call i32 @down_write_trylock(i8* %lock)
      ret void
    }
  )";
  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  auto it = module->getFunction("main")->front().begin();
  const Instruction *pthread_read_try = &*it++;
  const Instruction *pthread_write_try = &*it++;
  const Instruction *exclusive_try = &*it++;
  const Instruction *read_try = &*it++;
  const Instruction *write_try = &*it;
  EXPECT_TRUE(api->isTryLock(pthread_read_try));
  EXPECT_EQ(api->describeLockSemantics(pthread_read_try).mode,
            ThreadAPI::LockMode::Shared);
  EXPECT_TRUE(api->getLockSemanticInfo(pthread_read_try).conditional);
  EXPECT_TRUE(api->isTryLock(pthread_write_try));
  EXPECT_EQ(api->describeLockSemantics(pthread_write_try).mode,
            ThreadAPI::LockMode::Exclusive);
  EXPECT_TRUE(api->getLockSemanticInfo(pthread_write_try).conditional);
  EXPECT_TRUE(api->isTryLock(exclusive_try));
  EXPECT_EQ(api->describeLockSemantics(exclusive_try).mode,
            ThreadAPI::LockMode::Exclusive);
  EXPECT_TRUE(api->isTryLock(read_try));
  EXPECT_EQ(api->describeLockSemantics(read_try).mode,
            ThreadAPI::LockMode::Shared);
  EXPECT_TRUE(api->isTryLock(write_try));
  EXPECT_EQ(api->describeLockSemantics(write_try).mode,
            ThreadAPI::LockMode::Exclusive);
  EXPECT_EQ(api->getType(module->getFunction("atomic_read")),
            ThreadAPI::TD_KERNEL_ATOMIC_READ);
  EXPECT_EQ(api->getType(module->getFunction("atomic_set")),
            ThreadAPI::TD_KERNEL_ATOMIC_WRITE);
  EXPECT_EQ(api->getType(module->getFunction("atomic_cmpxchg")),
            ThreadAPI::TD_KERNEL_ATOMIC_RMW);
  EXPECT_EQ(api->getType(module->getFunction("test_and_set_bit")),
            ThreadAPI::TD_KERNEL_ATOMIC_RMW);
}

TEST_F(ThreadAPITest, RejectsSynchronizationAndCUDANearMissNames) {
  const char *source = R"(
    declare void @debug_mutex_lockEv(i8*)
    declare i32 @my_atomic_helper(i8*)
    declare void @copy_memcpy_stats(i8*)
  )";
  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  EXPECT_EQ(api->getType(module->getFunction("debug_mutex_lockEv")),
            ThreadAPI::TD_DUMMY);
  EXPECT_EQ(api->getType(module->getFunction("my_atomic_helper")),
            ThreadAPI::TD_DUMMY);
  EXPECT_EQ(api->getType(module->getFunction("copy_memcpy_stats")),
            ThreadAPI::TD_DUMMY);
}

TEST_F(ThreadAPITest, OpenMPInitPredicatesAreOperationSpecific) {
  EXPECT_TRUE(OpenMPModel::isTargetInit("__tgt_target"));
  EXPECT_FALSE(OpenMPModel::isTargetInit("__tgt_target_data_begin"));
  EXPECT_TRUE(OpenMPModel::isInteropInit("__kmpc_interop_init"));
  EXPECT_FALSE(OpenMPModel::isInteropInit("__kmpc_interop_fini"));
  EXPECT_TRUE(OpenMPModel::isDoacrossInit("__kmpc_doacross_init"));
  EXPECT_FALSE(OpenMPModel::isDoacrossInit("__kmpc_doacross_wait"));
  EXPECT_FALSE(OpenMPModel::isDoacrossInit("__kmpc_doacross_post"));
}

TEST_F(ThreadAPITest, KernelThreadLayoutsCanUseCallResults) {
  const char *source = R"(
    declare i8* @kthread_run(i8* (i8*)*, i8*, i8*)
    declare i32 @kthread_stop(i8*)
    define i8* @worker(i8* %arg) { ret i8* %arg }
    define void @main(i8* %data, i8* %name) {
      %task = call i8* @kthread_run(i8* (i8*)* @worker, i8* %data,
                                    i8* %name)
      %result = call i32 @kthread_stop(i8* %task)
      ret void
    }
  )";
  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  auto it = module->getFunction("main")->front().begin();
  const Instruction *fork = &*it++;
  const Instruction *join = &*it;

  EXPECT_EQ(api->getForkedThread(fork), fork);
  EXPECT_EQ(api->getForkedFun(fork), module->getFunction("worker"));
  EXPECT_EQ(api->getActualParmAtForkSite(fork),
            module->getFunction("main")->getArg(0));
  EXPECT_EQ(api->getJoinedThread(join), fork);
  EXPECT_EQ(api->getRetParmAtJoinedSite(join), join);
}

TEST_F(ThreadAPITest, CUDALaunchLayoutsDistinguishExAndLegacyForms) {
  const char *source = R"(
    declare i32 @cudaLaunchKernel(i8*, i8*, i8*, i8**, i64, i8*)
    declare i32 @cudaLaunchKernelExC(i8*, i8*, i8**)
    declare i32 @cuLaunchKernel(i8*, i32, i32, i32, i32, i32, i32, i32,
                                i8*, i8**)
    declare i32 @cuLaunchKernelEx_v2(i8*, i8*, i8**)
    define void @kernel() { ret void }
    define void @main(i8** %args) {
      call i32 @cudaLaunchKernel(i8* bitcast (void ()* @kernel to i8*),
                                 i8* null, i8* null, i8** %args, i64 0,
                                 i8* null)
      call i32 @cudaLaunchKernelExC(i8* null,
                                    i8* bitcast (void ()* @kernel to i8*),
                                    i8** %args)
      call i32 @cuLaunchKernel(i8* bitcast (void ()* @kernel to i8*),
                               i32 1, i32 1, i32 1, i32 1, i32 1, i32 1,
                               i32 0, i8* null, i8** %args)
      call i32 @cuLaunchKernelEx_v2(i8* null,
                                    i8* bitcast (void ()* @kernel to i8*),
                                    i8** %args)
      ret void
    }
  )";
  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  auto it = module->getFunction("main")->front().begin();
  for (unsigned i = 0; i < 4; ++i) {
    const Instruction *launch = &*it++;
    EXPECT_EQ(api->getCUDALaunchedKernel(launch),
              module->getFunction("kernel"));
    auto payload = api->getForkPayloadArgs(launch);
    ASSERT_EQ(payload.size(), 1u);
    EXPECT_EQ(payload.front(), module->getFunction("main")->getArg(0));
  }
}

TEST_F(ThreadAPITest, WrapperMembersUseUnderlyingMutexAndCorrectMode) {
  const char *source = R"(
    declare void @_ZNSt11unique_lockISt5mutexEC1ERS0_(i8*, i8*)
    declare void @_ZNSt11unique_lockISt5mutexE4lockEv(i8*)
    declare i1 @_ZNSt11unique_lockISt5mutexE8try_lockEv(i8*)
    declare void @_ZNSt11unique_lockISt5mutexE6unlockEv(i8*)
    declare void @_ZNSt11shared_lockISt12shared_mutexEC1ERS0_(i8*, i8*)
    declare void @_ZNSt11shared_lockISt12shared_mutexE4lockEv(i8*)
    declare i1 @_ZNSt11shared_lockISt12shared_mutexE8try_lockEv(i8*)
    declare void @_ZNSt11shared_lockISt12shared_mutexE6unlockEv(i8*)
    @mutex = global i8 0
    @shared = global i8 0
    define void @main() {
      %u = alloca i8
      %s = alloca i8
      call void @_ZNSt11unique_lockISt5mutexEC1ERS0_(i8* %u, i8* @mutex)
      call void @_ZNSt11unique_lockISt5mutexE4lockEv(i8* %u)
      call i1 @_ZNSt11unique_lockISt5mutexE8try_lockEv(i8* %u)
      call void @_ZNSt11unique_lockISt5mutexE6unlockEv(i8* %u)
      call void @_ZNSt11shared_lockISt12shared_mutexEC1ERS0_(i8* %s,
                                                             i8* @shared)
      call void @_ZNSt11shared_lockISt12shared_mutexE4lockEv(i8* %s)
      call i1 @_ZNSt11shared_lockISt12shared_mutexE8try_lockEv(i8* %s)
      call void @_ZNSt11shared_lockISt12shared_mutexE6unlockEv(i8* %s)
      ret void
    }
  )";
  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  std::vector<const Instruction *> calls;
  for (const Instruction &inst : instructions(*module->getFunction("main")))
    if (isa<CallBase>(inst))
      calls.push_back(&inst);
  ASSERT_EQ(calls.size(), 8u);

  for (unsigned i : {1u, 2u, 3u})
    EXPECT_EQ(api->getAnalysisLockIdentity(calls[i]),
              module->getNamedGlobal("mutex"));
  for (unsigned i : {5u, 6u, 7u})
    EXPECT_EQ(api->getAnalysisLockIdentity(calls[i]),
              module->getNamedGlobal("shared"));
  EXPECT_EQ(api->getType(api->getCallee(calls[1])),
            ThreadAPI::TD_UNIQUE_LOCK_LOCK);
  EXPECT_EQ(api->getType(api->getCallee(calls[5])),
            ThreadAPI::TD_SHARED_RDLOCK);
  EXPECT_EQ(api->describeLockSemantics(calls[5]).mode,
            ThreadAPI::LockMode::Shared);
  EXPECT_EQ(api->getLockSemanticInfo(calls[2]).try_success,
            ThreadAPI::TryLockSuccess::NonZero);
  EXPECT_EQ(api->getLockSemanticInfo(calls[6]).try_success,
            ThreadAPI::TryLockSuccess::NonZero);
}

TEST_F(ThreadAPITest, SharedMutexUnlockModeMatchesTheMemberOperation) {
  const char *source = R"(
    declare void @_ZNSt12shared_mutex6unlockEv(i8*)
    declare void @_ZNSt12shared_mutex13unlock_sharedEv(i8*)
    define void @main(i8* %lock) {
      call void @_ZNSt12shared_mutex6unlockEv(i8* %lock)
      call void @_ZNSt12shared_mutex13unlock_sharedEv(i8* %lock)
      ret void
    }
  )";
  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  auto it = module->getFunction("main")->front().begin();
  EXPECT_EQ(api->describeLockSemantics(&*it++).mode,
            ThreadAPI::LockMode::Exclusive);
  EXPECT_EQ(api->describeLockSemantics(&*it).mode,
            ThreadAPI::LockMode::Shared);
}

TEST_F(ThreadAPITest, WrapperLookupDoesNotUseLaterReconstruction) {
  const char *source = R"(
    declare void @fake_unique_lock_C1E(i8*, i8*)
    declare void @fake_unique_lock_unlockEv(i8*)
    @first = global i8 0
    @second = global i8 0
    define void @main() {
      %wrapper = alloca i8
      call void @fake_unique_lock_C1E(i8* %wrapper, i8* @first)
      call void @fake_unique_lock_unlockEv(i8* %wrapper)
      call void @fake_unique_lock_C1E(i8* %wrapper, i8* @second)
      ret void
    }
  )";
  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  auto it = module->getFunction("main")->front().begin();
  ++it;
  ++it;
  EXPECT_EQ(api->getAnalysisLockIdentity(&*it),
            module->getNamedGlobal("first"));
}

TEST_F(ThreadAPITest, TryAndConditionalAcquirePolarityIsPerAPI) {
  const char *source = R"(
    declare i32 @pthread_mutex_trylock(i8*)
    declare i1 @_ZNSt5mutex8try_lockEv(i8*)
    declare i32 @omp_test_lock(i8*)
    declare i32 @down_trylock(i8*)
    declare i32 @down_read_trylock(i8*)
    declare i32 @mutex_lock_interruptible(i8*)
    declare i32 @pthread_mutex_timedlock(i8*, i8*)
    define void @main(i8* %lock) {
      call i32 @pthread_mutex_trylock(i8* %lock)
      call i1 @_ZNSt5mutex8try_lockEv(i8* %lock)
      call i32 @omp_test_lock(i8* %lock)
      call i32 @down_trylock(i8* %lock)
      call i32 @down_read_trylock(i8* %lock)
      call i32 @mutex_lock_interruptible(i8* %lock)
      call i32 @pthread_mutex_timedlock(i8* %lock, i8* null)
      ret void
    }
  )";
  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  const std::vector<ThreadAPI::TryLockSuccess> expected = {
      ThreadAPI::TryLockSuccess::Zero, ThreadAPI::TryLockSuccess::NonZero,
      ThreadAPI::TryLockSuccess::NonZero, ThreadAPI::TryLockSuccess::Zero,
      ThreadAPI::TryLockSuccess::NonZero, ThreadAPI::TryLockSuccess::Zero,
      ThreadAPI::TryLockSuccess::Zero};
  auto it = module->getFunction("main")->front().begin();
  for (ThreadAPI::TryLockSuccess success : expected) {
    const Instruction *call = &*it++;
    const auto info = api->getLockSemanticInfo(call);
    EXPECT_TRUE(info.conditional);
    EXPECT_EQ(info.try_success, success);
  }
}

TEST_F(ThreadAPITest, OpenMPTargetDataPrefixesAndAliasCallsAreRecognized) {
  const char *source = R"(
    declare void @__tgt_target_data_begin_mapper(i8*)
    declare void @__tgt_target_data_end_nowait_mapper(i8*)
    declare void @__tgt_target_data_update_mapper(i8*)
    declare void @__tgt_target_mapper(i8*)
    declare void @__kmpc_fork_call(i8*, i32, i8*, ...)
    @fork_alias = alias void (i8*, i32, i8*, ...),
        void (i8*, i32, i8*, ...)* @__kmpc_fork_call
    define void @main() {
      call void (i8*, i32, i8*, ...) @fork_alias(i8* null, i32 0,
                                                  i8* null)
      ret void
    }
  )";
  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  EXPECT_EQ(api->getType(module->getFunction("__tgt_target_data_begin_mapper")),
            ThreadAPI::TD_OMP_TARGET_DATA_BEGIN);
  EXPECT_EQ(
      api->getType(module->getFunction("__tgt_target_data_end_nowait_mapper")),
      ThreadAPI::TD_OMP_TARGET_DATA_END);
  EXPECT_EQ(api->getType(module->getFunction("__tgt_target_data_update_mapper")),
            ThreadAPI::TD_OMP_TARGET_DATA_UPDATE);
  EXPECT_EQ(api->getType(module->getFunction("__tgt_target_mapper")),
            ThreadAPI::TD_OMP_TARGET);
  const auto *call = dyn_cast<CallBase>(&module->getFunction("main")->front().front());
  ASSERT_NE(call, nullptr);
  EXPECT_TRUE(OpenMPModel::isFork(call));
  EXPECT_TRUE(api->isForkLike(call));
}

TEST_F(ThreadAPITest, CppRecognitionCoversFreeFunctionsWithoutSubstrings) {
  const char *source = R"(
    declare void @_ZSt5asyncIiEvv()
    declare void @_ZSt11atomic_waitIiEvPKSt6atomicIT_ES1_()
    declare void @_ZNKSt6atomicIiE4waitEi(i8*, i32)
    declare void @debug_latch_count_down()
    declare void @my_barrier_waitE()
    declare void @app_semaphore_releaseE()
    declare void @_ZNSt13__future_base12_State_baseV217_M_complete_asyncEv()
  )";
  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  EXPECT_EQ(api->getType(module->getFunction("_ZSt5asyncIiEvv")),
            ThreadAPI::TD_ASYNC);
  EXPECT_EQ(api->getType(module->getFunction(
                "_ZSt11atomic_waitIiEvPKSt6atomicIT_ES1_")),
            ThreadAPI::TD_ATOMIC_WAIT);
  EXPECT_EQ(api->getType(module->getFunction("_ZNKSt6atomicIiE4waitEi")),
            ThreadAPI::TD_ATOMIC_WAIT);
  for (const char *name : {"debug_latch_count_down", "my_barrier_waitE",
                           "app_semaphore_releaseE",
                           "_ZNSt13__future_base12_State_baseV217_M_complete_asyncEv"})
    EXPECT_EQ(api->getType(module->getFunction(name)), ThreadAPI::TD_DUMMY);
}

TEST_F(ThreadAPITest, OpenMPBookkeepingIsNotBarrierButCancelBarrierIs) {
  const char *source = R"(
    declare void @__kmpc_end_single(i8*, i32)
    declare void @__kmpc_for_static_fini(i8*, i32)
    declare void @__kmpc_end_sections(i8*, i32)
    declare void @__kmpc_dispatch_fini_4(i8*, i32)
    declare i32 @__kmpc_cancel_barrier(i8*, i32)
    define void @main() {
      call void @__kmpc_end_single(i8* null, i32 0)
      call void @__kmpc_for_static_fini(i8* null, i32 0)
      call void @__kmpc_end_sections(i8* null, i32 0)
      call void @__kmpc_dispatch_fini_4(i8* null, i32 0)
      call i32 @__kmpc_cancel_barrier(i8* null, i32 0)
      ret void
    }
  )";
  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  auto it = module->getFunction("main")->front().begin();
  for (unsigned i = 0; i < 4; ++i)
    EXPECT_FALSE(api->isTDBarWait(&*it++));
  const Instruction *cancel_barrier = &*it;
  EXPECT_EQ(api->getType(api->getLLVMCallSite(cancel_barrier)),
            ThreadAPI::TD_BAR_WAIT);
  EXPECT_TRUE(api->isTDBarWait(cancel_barrier));
  EXPECT_EQ(api->getBarrierVal(cancel_barrier), cancel_barrier);
}

TEST_F(ThreadAPITest, PublicAliasSpellingPrecedesPrivateImplementation) {
  const char *source = R"(
    define i32 @__libc_internal_lock(i8* %lock) { ret i32 0 }
    @pthread_mutex_lock = alias i32 (i8*),
        i32 (i8*)* @__libc_internal_lock
    define void @main(i8* %lock) {
      call i32 @pthread_mutex_lock(i8* %lock)
      ret void
    }
  )";
  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  const auto *call = dyn_cast<CallBase>(
      &module->getFunction("main")->front().front());
  ASSERT_NE(call, nullptr);
  EXPECT_EQ(api->getCallee(call), module->getFunction("__libc_internal_lock"));
  EXPECT_EQ(api->getType(call), ThreadAPI::TD_ACQUIRE);
  EXPECT_TRUE(api->isTDAcquire(call));
  EXPECT_EQ(api->getAnalysisLockIdentity(call),
            module->getFunction("main")->getArg(0));
}

TEST_F(ThreadAPITest, PublicForkAndJoinAliasesKeepConfiguredLayouts) {
  const char *source = R"(
    define i32 @private_create(i8** %thread, i8* %attr,
                               i8* (i8*)* %start, i8* %arg) {
      ret i32 0
    }
    define i32 @private_join(i8* %thread, i8** %result) { ret i32 0 }
    @pthread_create = alias i32 (i8**, i8*, i8* (i8*)*, i8*),
        i32 (i8**, i8*, i8* (i8*)*, i8*)* @private_create
    @pthread_join = alias i32 (i8*, i8**),
        i32 (i8*, i8**)* @private_join
    define i8* @worker(i8* %arg) { ret i8* %arg }
    define void @main(i8** %thread, i8* %arg, i8** %result) {
      call i32 @pthread_create(i8** %thread, i8* null,
                               i8* (i8*)* @worker, i8* %arg)
      %handle = load i8*, i8** %thread
      call i32 @pthread_join(i8* %handle, i8** %result)
      ret void
    }
  )";
  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  auto it = module->getFunction("main")->front().begin();
  const Instruction *fork = &*it++;
  ++it;
  const Instruction *join = &*it;
  EXPECT_TRUE(api->isForkLike(fork));
  EXPECT_EQ(api->getForkedThread(fork),
            module->getFunction("main")->getArg(0));
  EXPECT_EQ(api->getForkedFun(fork), module->getFunction("worker"));
  EXPECT_EQ(api->getActualParmAtForkSite(fork),
            module->getFunction("main")->getArg(1));
  EXPECT_TRUE(api->isJoinLike(join));
  EXPECT_EQ(api->getRetParmAtJoinedSite(join),
            module->getFunction("main")->getArg(2));
}

TEST_F(ThreadAPITest, WrappedRwTryLockUsesCanonicalConditionalDescriptor) {
  const char *source = R"(
    declare i32 @__wrap_pthread_rwlock_tryrdlock(i8*)
    define void @main(i8* %lock) {
      call i32 @__wrap_pthread_rwlock_tryrdlock(i8* %lock)
      ret void
    }
  )";
  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  const Instruction *call = &module->getFunction("main")->front().front();
  const auto semantics = api->getLockSemanticInfo(call);
  EXPECT_TRUE(api->isTryLock(call));
  EXPECT_TRUE(semantics.conditional);
  EXPECT_TRUE(semantics.isShared());
  EXPECT_EQ(semantics.try_success, ThreadAPI::TryLockSuccess::Zero);
}

TEST_F(ThreadAPITest, AggregateCUDALaunchIsRecognizedWithoutFakeOperands) {
  const char *source = R"(
    declare i32 @cuLaunchCooperativeKernelMultiDevice(i8*, i32, i32)
    define void @main(i8* %records) {
      call i32 @cuLaunchCooperativeKernelMultiDevice(i8* %records, i32 1,
                                                      i32 0)
      ret void
    }
  )";
  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  const Instruction *launch = &module->getFunction("main")->front().front();
  EXPECT_TRUE(api->isForkLike(launch));
  EXPECT_EQ(api->getType(api->getLLVMCallSite(launch)),
            ThreadAPI::TD_CUDA_MULTI_DEVICE_LAUNCH);
  EXPECT_EQ(api->getCUDALaunchedKernel(launch), nullptr);
  EXPECT_EQ(api->getForkedFun(launch), nullptr);
  EXPECT_TRUE(api->getForkPayloadArgs(launch).empty());
  EXPECT_EQ(api->getSemanticLoweringInfo(api->getCallee(launch)).kind,
            ThreadAPI::SemanticLoweringKind::RecognizedButUnmodeled);
}

TEST_F(ThreadAPITest, RecognizesParameterizedBarrierArriveMangling) {
  const char *source = R"(
    declare void @_ZNSt7barrierISt18__empty_completionE6arriveEl(i8*, i64)
  )";
  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  EXPECT_EQ(api->getType(module->getFunction(
                "_ZNSt7barrierISt18__empty_completionE6arriveEl")),
            ThreadAPI::TD_BARRIER_ARRIVE);
}

TEST_F(ThreadAPITest, KernelBitOperationsSeparateReadFromRmw) {
  const char *source = R"(
    declare i1 @test_bit(i64, i8*)
    declare void @set_bit(i64, i8*)
    declare void @clear_bit(i64, i8*)
    declare void @change_bit(i64, i8*)
    declare i1 @test_and_set_bit(i64, i8*)
    declare i1 @test_and_clear_bit(i64, i8*)
  )";
  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  EXPECT_EQ(api->getType(module->getFunction("test_bit")),
            ThreadAPI::TD_KERNEL_ATOMIC_READ);
  for (const char *name : {"set_bit", "clear_bit", "change_bit",
                           "test_and_set_bit", "test_and_clear_bit"})
    EXPECT_EQ(api->getType(module->getFunction(name)),
              ThreadAPI::TD_KERNEL_ATOMIC_RMW);
}

TEST_F(ThreadAPITest, ExpandedKthreadCreateWakeSequenceLowersAsFork) {
  const char *source = R"(
    declare i8* @kthread_create_on_node(i8* (i8*)*, i8*, i32, i8*, ...)
    declare i32 @wake_up_process(i8*)
    define i8* @worker(i8* %data) { ret i8* %data }
    define void @main(i8* %data, i8* %name) {
      %task = call i8* (i8* (i8*)*, i8*, i32, i8*, ...)
          @kthread_create_on_node(i8* (i8*)* @worker, i8* %data, i32 -1,
                                  i8* %name)
      call i32 @wake_up_process(i8* %task)
      ret void
    }
  )";
  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  auto it = module->getFunction("main")->front().begin();
  const Instruction *create = &*it++;
  const Instruction *wake = &*it;
  EXPECT_FALSE(api->isForkLike(create));
  EXPECT_TRUE(api->isForkLike(wake));
  EXPECT_EQ(api->getForkedThread(wake), create);
  EXPECT_EQ(api->getForkedFun(wake), module->getFunction("worker"));
  EXPECT_EQ(api->getActualParmAtForkSite(wake),
            module->getFunction("main")->getArg(0));
  auto payload = api->getForkPayloadArgs(wake);
  ASSERT_EQ(payload.size(), 1u);
  EXPECT_EQ(payload.front(), module->getFunction("main")->getArg(0));
}

TEST_F(ThreadAPITest, UnrelatedWakeUpProcessIsNotInventedAsFork) {
  const char *source = R"(
    declare i32 @wake_up_process(i8*)
    define void @main(i8* %existing_task) {
      call i32 @wake_up_process(i8* %existing_task)
      ret void
    }
  )";
  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  const Instruction *wake = &module->getFunction("main")->front().front();
  EXPECT_FALSE(api->isForkLike(wake));
  EXPECT_EQ(api->getForkedFun(wake), nullptr);
}
