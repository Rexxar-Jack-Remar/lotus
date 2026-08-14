TEST_F(MPIAnalysisTest, RMAExtractionHandlesLockAllAndAtomicOps) {
  const char *source = R"(
    declare i32 @MPI_Win_create(i8*, i64, i32, i8*, i8*)
    declare i32 @MPI_Win_lock_all(i32, i8*)
    declare i32 @MPI_Win_flush_all(i8*)
    declare i32 @MPI_Win_unlock_all(i8*)
    declare i32 @MPI_Fetch_and_op(i8*, i8*, i32, i32, i64, i32, i8*)
    declare i32 @MPI_Compare_and_swap(i8*, i8*, i8*, i32, i32, i64, i8*)

    define i32 @main(i8* %comm) {
    entry:
      %win = alloca i8, align 1
      call i32 @MPI_Win_create(i8* null, i64 16, i32 4, i8* %comm, i8* %win)
      call i32 @MPI_Win_lock_all(i32 0, i8* %win)
      call i32 @MPI_Fetch_and_op(i8* null, i8* null, i32 2, i32 3, i64 0, i32 0, i8* %win)
      call i32 @MPI_Compare_and_swap(i8* null, i8* null, i8* null, i32 2, i32 4, i64 8, i8* %win)
      call i32 @MPI_Win_flush_all(i8* %win)
      call i32 @MPI_Win_unlock_all(i8* %win)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();
  const auto &ops = analysis.getProcessModel().getAllOperations();

  const MPIOperation *fetch = findOperation(ops, ThreadAPI::TD_MPI_ACCUMULATE);
  ASSERT_NE(fetch, nullptr);
  EXPECT_EQ(fetch->target_rank, 3);
  EXPECT_NE(fetch->window, nullptr);
  EXPECT_EQ(fetch->byte_length, 4);

  const MPIOperation *lock = findOperation(ops, ThreadAPI::TD_MPI_WIN_LOCK);
  const MPIOperation *flush = findOperation(ops, ThreadAPI::TD_MPI_WIN_FLUSH);
  const MPIOperation *unlock = findOperation(ops, ThreadAPI::TD_MPI_WIN_UNLOCK);
  ASSERT_NE(lock, nullptr);
  ASSERT_NE(flush, nullptr);
  ASSERT_NE(unlock, nullptr);
  EXPECT_EQ(lock->window, fetch->window);
  EXPECT_EQ(flush->window, fetch->window);
  EXPECT_EQ(unlock->window, fetch->window);
}

TEST_F(MPIAnalysisTest, PMPI_RMAVariantsPreserveWindowAndTargetExtraction) {
  const char *source = R"(
    declare i32 @PMPI_Win_create(i8*, i64, i32, i8*, i8*)
    declare i32 @PMPI_Win_lock_all(i32, i8*)
    declare i32 @PMPI_Win_flush_all(i8*)
    declare i32 @PMPI_Win_unlock_all(i8*)
    declare i32 @PMPI_Fetch_and_op(i8*, i8*, i32, i32, i64, i32, i8*)

    define i32 @main(i8* %comm) {
    entry:
      %win = alloca i8, align 1
      call i32 @PMPI_Win_create(i8* null, i64 16, i32 4, i8* %comm, i8* %win)
      call i32 @PMPI_Win_lock_all(i32 0, i8* %win)
      call i32 @PMPI_Fetch_and_op(i8* null, i8* null, i32 2, i32 5, i64 8, i32 0, i8* %win)
      call i32 @PMPI_Win_flush_all(i8* %win)
      call i32 @PMPI_Win_unlock_all(i8* %win)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();
  const auto &ops = analysis.getProcessModel().getAllOperations();

  const MPIOperation *fetch = findOperation(ops, ThreadAPI::TD_MPI_ACCUMULATE);
  ASSERT_NE(fetch, nullptr);
  EXPECT_EQ(fetch->target_rank, 5);
  EXPECT_NE(fetch->window, nullptr);
  EXPECT_EQ(fetch->byte_length, 4);

  const MPIOperation *lock = findOperation(ops, ThreadAPI::TD_MPI_WIN_LOCK);
  const MPIOperation *flush = findOperation(ops, ThreadAPI::TD_MPI_WIN_FLUSH);
  const MPIOperation *unlock = findOperation(ops, ThreadAPI::TD_MPI_WIN_UNLOCK);
  ASSERT_NE(lock, nullptr);
  ASSERT_NE(flush, nullptr);
  ASSERT_NE(unlock, nullptr);
  EXPECT_EQ(lock->window, fetch->window);
  EXPECT_EQ(flush->window, fetch->window);
  EXPECT_EQ(unlock->window, fetch->window);
}

TEST_F(MPIAnalysisTest, WinAllocateVariantsUseCommunicatorOperandForClasses) {
  const char *source = R"(
    declare i32 @MPI_Win_allocate(i64, i32, i8*, i8*, i8*)
    declare i32 @MPI_Win_allocate_shared(i64, i32, i8*, i8*, i8*)

    define i32 @main(i8* %comm, i8* %win1, i8* %win2) {
    entry:
      call i32 @MPI_Win_allocate(i64 16, i32 4, i8* null, i8* %comm, i8* %win1)
      call i32 @MPI_Win_allocate_shared(i64 32, i32 4, i8* null, i8* %comm, i8* %win2)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();
  const auto &ops = analysis.getProcessModel().getAllOperations();

  size_t checked = 0;
  for (const MPIOperation &op : ops) {
    if (op.td_type != ThreadAPI::TD_MPI_WIN_CREATE) {
      continue;
    }
    ++checked;
    EXPECT_EQ(op.communicator, module->getFunction("main")->getArg(0));
    EXPECT_NE(op.communicator_class_id, 0u);
  }
  EXPECT_EQ(checked, 2u);
}

TEST_F(MPIAnalysisTest, CommDupWithInfoUsesNewCommunicatorResultSlot) {
  const char *source = R"(
    declare i32 @MPI_Comm_dup_with_info(i8*, i8*, i8*)
    declare i32 @MPI_Barrier(i8*)

    define i32 @main(i8* %comm, i8* %info, i8* %newcomm) {
    entry:
      call i32 @MPI_Comm_dup_with_info(i8* %comm, i8* %info, i8* %newcomm)
      call i32 @MPI_Barrier(i8* %newcomm)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();
  const auto &ops = analysis.getProcessModel().getAllOperations();

  const MPIOperation *barrier = findOperation(ops, ThreadAPI::TD_MPI_BARRIER);
  ASSERT_NE(barrier, nullptr);
  EXPECT_NE(barrier->communicator, module->getFunction("main")->getArg(1));
  EXPECT_NE(barrier->communicator_class_id, 0u);
}

TEST_F(MPIAnalysisTest, CommIdupProducesPendingRequestAndDerivedCommunicator) {
  const char *source = R"(
    declare i32 @MPI_Comm_idup(i8*, i8*, i8*)
    declare i32 @MPI_Barrier(i8*)

    define i32 @main(i8* %comm, i8* %newcomm, i8* %req) {
    entry:
      call i32 @MPI_Comm_idup(i8* %comm, i8* %newcomm, i8* %req)
      call i32 @MPI_Barrier(i8* %newcomm)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  EXPECT_EQ(analysis.getResults().orphaned_requests.size(), 1u);
  const auto &summaries = analysis.getProcessModel().getRequestStateSummaries();
  ASSERT_EQ(summaries.size(), 1u);
  const auto &summary = summaries.begin()->second;
  EXPECT_EQ(summary.state, MPIRequestState::Active);
  EXPECT_EQ(summary.origin_inst, &module->getFunction("main")->getEntryBlock().front());

  const auto &ops = analysis.getProcessModel().getAllOperations();
  const MPIOperation *barrier = findOperation(ops, ThreadAPI::TD_MPI_BARRIER);
  ASSERT_NE(barrier, nullptr);
  EXPECT_NE(barrier->communicator, nullptr);
  EXPECT_NE(barrier->communicator_class_id, 0u);
}

TEST_F(MPIAnalysisTest, CommIdupWaitCompletesRequestLifecycle) {
  const char *source = R"(
    declare i32 @MPI_Comm_idup(i8*, i8*, i8*)
    declare i32 @MPI_Wait(i8*, i8*)

    define i32 @main(i8* %comm, i8* %newcomm, i8* %req) {
    entry:
      call i32 @MPI_Comm_idup(i8* %comm, i8* %newcomm, i8* %req)
      call i32 @MPI_Wait(i8* %req, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  EXPECT_TRUE(analysis.getResults().orphaned_requests.empty());
  const auto &summaries = analysis.getProcessModel().getRequestStateSummaries();
  ASSERT_EQ(summaries.size(), 1u);
  EXPECT_EQ(summaries.begin()->second.state, MPIRequestState::MustComplete);
}

TEST_F(MPIAnalysisTest, CommIdupTestFalseKeepsRequestPendingUntilFreed) {
  const char *source = R"(
    declare i32 @MPI_Comm_idup(i8*, i8*, i8*)
    declare i32 @MPI_Test(i8*, i32*, i8*)
    declare i32 @MPI_Request_free(i8*)

    define i32 @main(i8* %comm, i8* %newcomm, i8* %req) {
    entry:
      %flag = alloca i32, align 4
      store i32 0, i32* %flag, align 4
      call i32 @MPI_Comm_idup(i8* %comm, i8* %newcomm, i8* %req)
      call i32 @MPI_Test(i8* %req, i32* %flag, i8* null)
      call i32 @MPI_Request_free(i8* %req)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  EXPECT_TRUE(analysis.getResults().orphaned_requests.empty());
  const auto &summaries = analysis.getProcessModel().getRequestStateSummaries();
  ASSERT_EQ(summaries.size(), 1u);
  EXPECT_EQ(summaries.begin()->second.state, MPIRequestState::Freed);
  const auto &deferred = analysis.getProcessModel().getDeferredLoweringStats();
  auto it = deferred.find("test_unknown_flag");
  if (it != deferred.end()) {
    EXPECT_EQ(it->second, 0u);
  }
}

TEST_F(MPIAnalysisTest, CommSplitTypeDoesNotTreatSplitTypeAsColorIdentity) {
  const char *source = R"(
    declare i32 @MPI_Comm_split_type(i8*, i32, i32, i8*, i8*)
    declare i32 @MPI_Barrier(i8*)

    define i32 @main(i8* %comm, i8* %info, i8* %newcomm) {
    entry:
      call i32 @MPI_Comm_split_type(i8* %comm, i32 1, i32 0, i8* %info, i8* %newcomm)
      call i32 @MPI_Barrier(i8* %newcomm)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  bool saw_gap = false;
  for (const auto &gap : analysis.getResults().model_gaps) {
    if (gap.code == "mpi_subgroup_identity_unresolved") {
      saw_gap = true;
      break;
    }
  }
  EXPECT_TRUE(saw_gap);

  const auto &ops = analysis.getProcessModel().getAllOperations();
  const MPIOperation *barrier = findOperation(ops, ThreadAPI::TD_MPI_BARRIER);
  ASSERT_NE(barrier, nullptr);
  EXPECT_NE(barrier->communicator_class_id, 0u);
}

TEST_F(MPIAnalysisTest, TopologyCommunicatorCreationRegistersDerivedHandle) {
  const char *source = R"(
    declare i32 @MPI_Cart_create(i8*, i32, i32*, i32*, i32, i8*)
    declare i32 @MPI_Barrier(i8*)

    define i32 @main(i8* %comm, i32* %dims, i32* %periods, i8* %newcomm) {
    entry:
      call i32 @MPI_Cart_create(i8* %comm, i32 1, i32* %dims, i32* %periods, i32 0, i8* %newcomm)
      call i32 @MPI_Barrier(i8* %newcomm)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();
  const auto &ops = analysis.getProcessModel().getAllOperations();

  const MPIOperation *barrier = findOperation(ops, ThreadAPI::TD_MPI_BARRIER);
  ASSERT_NE(barrier, nullptr);
  EXPECT_NE(barrier->communicator_class_id, 0u);
}
