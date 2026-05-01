#include "ConcurrencyCheckerTestSupport.h"

TEST_F(ConcurrencyCheckerTest,
       LaterNonReleaseStoreDoesNotSuppressRaceThroughReleaseHB) {
  const char *source = R"(
    @shared = global i32 0, align 4
    @flag = global i32 0, align 4

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)

    define i8* @writer(i8* %arg) {
    entry:
      store i32 1, i32* @shared, align 4
      store atomic i32 1, i32* @flag release, align 4
      ret i8* null
    }

    define i8* @overwriter(i8* %arg) {
    entry:
      store atomic i32 2, i32* @flag monotonic, align 4
      ret i8* null
    }

    define i8* @reader(i8* %arg) {
    entry:
      %seen = load atomic i32, i32* @flag acquire, align 4
      %ready = icmp ne i32 %seen, 0
      br i1 %ready, label %write, label %exit

    write:
      store i32 %seen, i32* @shared, align 4
      br label %exit

    exit:
      ret i8* null
    }

    define i32 @main() {
    entry:
      %tid1 = alloca i8, align 1
      %tid2 = alloca i8, align 1
      %tid3 = alloca i8, align 1
      call i32 @pthread_create(i8* %tid1, i8* null, i8* (i8*)* @writer, i8* null)
      call i32 @pthread_create(i8* %tid2, i8* null, i8* (i8*)* @overwriter, i8* null)
      call i32 @pthread_create(i8* %tid3, i8* null, i8* (i8*)* @reader, i8* null)
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
  lotus::HappensBeforeAnalysis hb(*module, mhp);
  hb.setAliasAnalysis(mhp.getAliasAnalysis());
  hb.analyze();

  concurrency::DataRaceChecker checker(*module, &mhp, mhp.getLockSetAnalysis(),
                                       &escape, nullptr, nullptr,
                                       mhp.getAliasAnalysis(), &hb);

  const Instruction *store1 = nullptr;
  const Instruction *store2 = nullptr;
  for (const Instruction &inst : instructions(*module->getFunction("writer"))) {
    if (isa<StoreInst>(&inst)) {
      store1 = &inst;
      break;
    }
  }
  for (const Instruction &inst : instructions(*module->getFunction("reader"))) {
    if (isa<StoreInst>(&inst)) {
      store2 = &inst;
      break;
    }
  }

  ASSERT_NE(store1, nullptr);
  ASSERT_NE(store2, nullptr);
  EXPECT_TRUE(checker.wouldReportDataRace(store1, store2));
}
TEST_F(ConcurrencyCheckerTest,
       InitialAtomicValueWitnessDoesNotSuppressPayloadRace) {
  const char *source = R"(
    @shared = global i32 0, align 4
    @flag = global i32 1, align 4

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)

    define i8* @writer(i8* %arg) {
    entry:
      store i32 1, i32* @shared, align 4
      store atomic i32 1, i32* @flag release, align 4
      ret i8* null
    }

    define i8* @reader(i8* %arg) {
    entry:
      %seen = load atomic i32, i32* @flag acquire, align 4
      %ready = icmp eq i32 %seen, 1
      br i1 %ready, label %write, label %exit

    write:
      store i32 %seen, i32* @shared, align 4
      br label %exit

    exit:
      ret i8* null
    }

    define i32 @main() {
    entry:
      %tid1 = alloca i8, align 1
      %tid2 = alloca i8, align 1
      call i32 @pthread_create(i8* %tid1, i8* null, i8* (i8*)* @writer, i8* null)
      call i32 @pthread_create(i8* %tid2, i8* null, i8* (i8*)* @reader, i8* null)
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
  lotus::HappensBeforeAnalysis hb(*module, mhp);
  hb.setAliasAnalysis(mhp.getAliasAnalysis());
  hb.analyze();

  concurrency::DataRaceChecker checker(*module, &mhp, mhp.getLockSetAnalysis(),
                                       &escape, nullptr, nullptr,
                                       mhp.getAliasAnalysis(), &hb);

  const Instruction *store1 = nullptr;
  const Instruction *store2 = nullptr;
  for (const Instruction &inst : instructions(*module->getFunction("writer"))) {
    if (isa<StoreInst>(&inst)) {
      store1 = &inst;
      break;
    }
  }
  for (const Instruction &inst : instructions(*module->getFunction("reader"))) {
    if (const auto *store = dyn_cast<StoreInst>(&inst)) {
      if (store->getPointerOperand() == module->getNamedGlobal("shared")) {
        store2 = &inst;
      }
    }
  }

  ASSERT_NE(store1, nullptr);
  ASSERT_NE(store2, nullptr);
  EXPECT_TRUE(checker.wouldReportDataRace(store1, store2));
}
TEST_F(ConcurrencyCheckerTest,
       PthreadGetspecificDerivedPointersAreNotPrunedAsThreadLocal) {
  const char *source = R"(
    declare i8* @pthread_getspecific(i32)
    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)

    define i8* @worker1(i8* %arg) {
    entry:
      %tls = call i8* @pthread_getspecific(i32 0)
      %typed = bitcast i8* %tls to i32*
      store i32 1, i32* %typed, align 4
      ret i8* null
    }

    define i8* @worker2(i8* %arg) {
    entry:
      %tls = call i8* @pthread_getspecific(i32 0)
      %typed = bitcast i8* %tls to i32*
      store i32 2, i32* %typed, align 4
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
  mhp.analyze();

  lotus::EscapeAnalysis escape(*module);
  escape.analyze();
  ThreadLocal::ThreadLocalAnalysis threadLocal(*module);
  threadLocal.analyze();

  concurrency::DataRaceChecker checker(*module, &mhp, mhp.getLockSetAnalysis(),
                                       &escape, &threadLocal, nullptr,
                                       mhp.getAliasAnalysis(), nullptr);

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
TEST_F(ConcurrencyCheckerTest,
       UnresolvedIndirectCallDoesNotPreserveOptimisticLockSuppression) {
  const char *source = R"(
    @hook = external global void ()*
    @lock = global i8 0
    @shared = global i32 0, align 4

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare i32 @pthread_mutex_lock(i8*)
    declare i32 @pthread_mutex_unlock(i8*)

    define void @unlock_helper() {
    entry:
      call i32 @pthread_mutex_unlock(i8* @lock)
      ret void
    }

    define i8* @worker1(i8* %arg) {
    entry:
      call i32 @pthread_mutex_lock(i8* @lock)
      %fn = load void ()*, void ()** @hook
      call void %fn()
      store i32 1, i32* @shared, align 4
      ret i8* null
    }

    define i8* @worker2(i8* %arg) {
    entry:
      call i32 @pthread_mutex_lock(i8* @lock)
      %fn = load void ()*, void ()** @hook
      call void %fn()
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
                                       &escape, nullptr, nullptr,
                                       mhp.getAliasAnalysis(), nullptr);

  const Instruction *store1 = nullptr;
  const Instruction *store2 = nullptr;
  for (const Instruction &inst :
       instructions(*module->getFunction("worker1"))) {
    if (isa<StoreInst>(&inst)) {
      store1 = &inst;
    }
  }
  for (const Instruction &inst :
       instructions(*module->getFunction("worker2"))) {
    if (isa<StoreInst>(&inst)) {
      store2 = &inst;
    }
  }

  ASSERT_NE(store1, nullptr);
  ASSERT_NE(store2, nullptr);
  EXPECT_TRUE(checker.wouldReportDataRace(store1, store2));
}
TEST_F(ConcurrencyCheckerTest, SuppressesRaceForOpenMPPrivateLikeCaptures) {
  const char *source = R"(
    declare i32 @__kmpc_omp_task(i8*, i32, i8*)

    define internal void @task1(i32* %.omp.private_buf) {
    entry:
      %tmp = alloca i32*, align 8
      store i32* %.omp.private_buf, i32** %tmp, align 8
      %loaded = load i32*, i32** %tmp, align 8
      %elt = getelementptr inbounds i32, i32* %loaded, i64 1
      store i32 1, i32* %elt, align 4
      ret void
    }

    define internal void @task2(i32* %.omp.private_buf) {
    entry:
      %tmp = alloca i32*, align 8
      store i32* %.omp.private_buf, i32** %tmp, align 8
      %loaded = load i32*, i32** %tmp, align 8
      %elt = getelementptr inbounds i32, i32* %loaded, i64 1
      store i32 2, i32* %elt, align 4
      ret void
    }

    define i32 @main() {
    entry:
      %t1 = call i32 @__kmpc_omp_task(
          i8* null, i32 0, i8* bitcast (void (i32*)* @task1 to i8*))
      %t2 = call i32 @__kmpc_omp_task(
          i8* null, i32 0, i8* bitcast (void (i32*)* @task2 to i8*))
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  mhp::MHPAnalysis mhp(*module);
  mhp.analyze();

  lotus::EscapeAnalysis escape(*module);
  escape.analyze();

  concurrency::DataRaceChecker checker(*module, &mhp, mhp.getLockSetAnalysis(),
                                       &escape, nullptr, nullptr,
                                       mhp.getAliasAnalysis(), nullptr);

  const Instruction *store1 = nullptr;
  const Instruction *store2 = nullptr;
  for (const Instruction &inst : instructions(*module->getFunction("task1"))) {
    if (isa<StoreInst>(&inst)) {
      store1 = &inst;
      break;
    }
  }
  for (const Instruction &inst : instructions(*module->getFunction("task2"))) {
    if (isa<StoreInst>(&inst)) {
      store2 = &inst;
      break;
    }
  }

  ASSERT_NE(store1, nullptr);
  ASSERT_NE(store2, nullptr);
  EXPECT_FALSE(checker.wouldReportDataRace(store1, store2));
}
TEST_F(ConcurrencyCheckerTest, SuppressesRaceInsideNamedOpenMPCriticalSection) {
  const char *source = R"(
    @shared = global i32 0, align 4
    @crit = global [8 x i32] zeroinitializer

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare void @__kmpc_critical(i8*, i32, [8 x i32]*)
    declare void @__kmpc_end_critical(i8*, i32, [8 x i32]*)

    define i8* @worker1(i8* %arg) {
    entry:
      call void @__kmpc_critical(i8* null, i32 0, [8 x i32]* @crit)
      store i32 1, i32* @shared, align 4
      call void @__kmpc_end_critical(i8* null, i32 0, [8 x i32]* @crit)
      ret i8* null
    }

    define i8* @worker2(i8* %arg) {
    entry:
      call void @__kmpc_critical(i8* null, i32 0, [8 x i32]* @crit)
      store i32 2, i32* @shared, align 4
      call void @__kmpc_end_critical(i8* null, i32 0, [8 x i32]* @crit)
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
                                       &escape, nullptr, nullptr,
                                       mhp.getAliasAnalysis(), nullptr);

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
  EXPECT_FALSE(checker.wouldReportDataRace(store1, store2));
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
TEST_F(ConcurrencyCheckerTest, DetectsMPIInvalidTagBug) {
  const char *source = R"(
    declare i32 @MPI_Send(i8*, i32, i32, i32, i32, i8*)

    define void @test_mpi_invalid_tag(i8* %buf, i8* %comm) {
      %call = call i32 @MPI_Send(i8* %buf, i32 1, i32 0, i32 1, i32 -1, i8* %comm)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  concurrency::MPIChecker checker(*module);
  auto reports = checker.checkMPIBugs();

  bool found = false;
  for (const auto &report : reports) {
    if (report.bugType == concurrency::ConcurrencyBugType::MPI_INVALID_TAG) {
      found = true;
      break;
    }
  }
  EXPECT_TRUE(found);
}
TEST_F(ConcurrencyCheckerTest, DetectsMPIWindowLifecycleBugs) {
  const char *source = R"(
    @win = global i8 0, align 1

    declare i32 @MPI_Win_create(i8*, i64, i32, i8*, i8*)
    declare i32 @MPI_Win_free(i8*)
    declare i32 @MPI_Win_unlock(i32, i8*)
    declare i32 @MPI_Put(i8*, i32, i32, i32, i64, i32, i32, i8*)

    define i32 @main(i8* %comm) {
    entry:
      call i32 @MPI_Win_create(i8* null, i64 8, i32 4, i8* %comm, i8* @win)
      call i32 @MPI_Win_free(i8* @win)
      call i32 @MPI_Win_free(i8* @win)
      call i32 @MPI_Put(i8* null, i32 1, i32 0, i32 1, i64 0, i32 1, i32 0, i8* @win)
      call i32 @MPI_Win_unlock(i32 1, i8* @win)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  concurrency::MPIChecker checker(*module);
  auto reports = checker.checkMPIBugs();

  bool found_invalid_transition = false;
  bool found_use_after_free = false;
  bool found_double_free = false;
  for (const auto &report : reports) {
    if (report.bugType ==
        concurrency::ConcurrencyBugType::MPI_INVALID_RMA_TRANSITION) {
      found_invalid_transition = true;
    }
    if (report.bugType ==
        concurrency::ConcurrencyBugType::MPI_USE_AFTER_FREE_WINDOW) {
      found_use_after_free = true;
    }
    if (report.bugType ==
        concurrency::ConcurrencyBugType::MPI_DOUBLE_WINDOW_FREE) {
      found_double_free = true;
    }
  }

  EXPECT_TRUE(found_invalid_transition);
  EXPECT_TRUE(found_use_after_free);
  EXPECT_TRUE(found_double_free);
}
TEST_F(ConcurrencyCheckerTest, DetectsAdditionalMPIMappingBugs) {
  const char *source = R"(
    declare i32 @MPI_Send(i8*, i32, i32, i32, i32, i8*)
    declare i32 @MPI_Recv(i8*, i32, i32, i32, i32, i8*, i8*)
    declare i32 @MPI_Isend(i8*, i32, i32, i32, i32, i8*, i8*)
    declare i32 @MPI_Request_free(i8*)
    declare i32 @MPI_Wait(i8*, i8*)
    declare i32 @MPI_Bcast(i8*, i32, i32, i32, i8*)
    declare i32 @MPI_Comm_free(i8*)

    @MPI_IN_PLACE = external global i8
    @MPI_COMM_NULL = external global i8

    define i32 @main(i8* %comm) {
    entry:
      %req = alloca i8, align 1
      call i32 @MPI_Send(i8* null, i32 1, i32 0, i32 -3, i32 7, i8* %comm)
      call i32 @MPI_Send(i8* null, i32 1, i32 0, i32 1, i32 7, i8* %comm)
      call i32 @MPI_Recv(i8* null, i32 2, i32 0, i32 1, i32 7, i8* %comm, i8* null)
      call i32 @MPI_Isend(i8* null, i32 1, i32 0, i32 1, i32 9, i8* %comm, i8* %req)
      call i32 @MPI_Wait(i8* %req, i8* null)
      call i32 @MPI_Request_free(i8* %req)
      call i32 @MPI_Bcast(i8* @MPI_IN_PLACE, i32 1, i32 0, i32 -1, i8* %comm)
      call i32 @MPI_Comm_free(i8* @MPI_COMM_NULL)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  concurrency::MPIChecker checker(*module);
  auto reports = checker.checkMPIBugs();

  bool found_invalid_rank = false;
  bool found_type_size_mismatch = false;
  bool found_destroy_null_comm = false;
  bool found_request_free_after_wait = false;
  bool found_in_place_wrong_op = false;

  for (const auto &report : reports) {
    if (report.bugType == concurrency::ConcurrencyBugType::MPI_INVALID_RANK) {
      found_invalid_rank = true;
    }
    if (report.bugType ==
        concurrency::ConcurrencyBugType::MPI_TYPE_SIZE_MISMATCH) {
      found_type_size_mismatch = true;
    }
    if (report.bugType ==
        concurrency::ConcurrencyBugType::MPI_DESTROY_NULL_COMM) {
      found_destroy_null_comm = true;
    }
    if (report.bugType ==
        concurrency::ConcurrencyBugType::MPI_REQUEST_FREE_AFTER_WAIT) {
      found_request_free_after_wait = true;
    }
    if (report.bugType ==
        concurrency::ConcurrencyBugType::MPI_IN_PLACE_WRONG_OP) {
      found_in_place_wrong_op = true;
    }
  }

  EXPECT_TRUE(found_invalid_rank);
  EXPECT_TRUE(found_type_size_mismatch);
  EXPECT_TRUE(found_destroy_null_comm);
  EXPECT_TRUE(found_request_free_after_wait);
  EXPECT_TRUE(found_in_place_wrong_op);
}
