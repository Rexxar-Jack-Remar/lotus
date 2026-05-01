#include "ThreadAPITestSupport.h"

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
