TEST_F(MPIAnalysisTest, BlockingSendCycleNeedsCompatibleReceives) {
  const char *source = R"(
    declare i32 @MPI_Send(i8*, i32, i32, i32, i32, i8*)
    declare i32 @MPI_Recv(i8*, i32, i32, i32, i32, i8*, i8*)

    define i32 @rank0(i8* %comm) {
    entry:
      call i32 @MPI_Send(i8* null, i32 1, i32 0, i32 1, i32 9, i8* %comm)
      call i32 @MPI_Recv(i8* null, i32 1, i32 0, i32 1, i32 7, i8* %comm, i8* null)
      ret i32 0
    }

    define i32 @rank1(i8* %comm) {
    entry:
      call i32 @MPI_Send(i8* null, i32 1, i32 0, i32 0, i32 7, i8* %comm)
      call i32 @MPI_Recv(i8* null, i32 1, i32 0, i32 0, i32 9, i8* %comm, i8* null)
      ret i32 0
    }

    define i32 @main(i8* %comm) {
    entry:
      %a = call i32 @rank0(i8* %comm)
      %b = call i32 @rank1(i8* %comm)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  EXPECT_EQ(analysis.getResults().potential_deadlocks.size(), 1u);
}

TEST_F(MPIAnalysisTest, IncompatibleLaterReceivesDoNotTriggerDeadlock) {
  const char *source = R"(
    declare i32 @MPI_Send(i8*, i32, i32, i32, i32, i8*)
    declare i32 @MPI_Recv(i8*, i32, i32, i32, i32, i8*, i8*)

    define i32 @rank0(i8* %comm) {
    entry:
      call i32 @MPI_Send(i8* null, i32 1, i32 0, i32 1, i32 9, i8* %comm)
      call i32 @MPI_Recv(i8* null, i32 1, i32 0, i32 1, i32 99, i8* %comm, i8* null)
      ret i32 0
    }

    define i32 @rank1(i8* %comm) {
    entry:
      call i32 @MPI_Send(i8* null, i32 1, i32 0, i32 0, i32 7, i8* %comm)
      call i32 @MPI_Recv(i8* null, i32 1, i32 0, i32 0, i32 98, i8* %comm, i8* null)
      ret i32 0
    }

    define i32 @main(i8* %comm) {
    entry:
      %a = call i32 @rank0(i8* %comm)
      %b = call i32 @rank1(i8* %comm)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  EXPECT_TRUE(analysis.getResults().potential_deadlocks.empty());
}

TEST_F(MPIAnalysisTest, BlockingWaitDoesNotPrematurelyDischargeChannel) {
  const char *source = R"(
    declare i32 @MPI_Irecv(i8*, i32, i32, i32, i32, i8*, i8*)
    declare i32 @MPI_Wait(i8*, i8*)
    declare i32 @MPI_Send(i8*, i32, i32, i32, i32, i8*)

    define i32 @rank0(i8* %comm) {
    entry:
      %req = alloca i8, align 1
      call i32 @MPI_Irecv(i8* null, i32 1, i32 0, i32 1, i32 1, i8* %comm, i8* %req)
      call i32 @MPI_Wait(i8* %req, i8* null)
      call i32 @MPI_Send(i8* null, i32 1, i32 0, i32 1, i32 2, i8* %comm)
      ret i32 0
    }

    define i32 @rank1(i8* %comm) {
    entry:
      %req = alloca i8, align 1
      call i32 @MPI_Irecv(i8* null, i32 1, i32 0, i32 0, i32 2, i8* %comm, i8* %req)
      call i32 @MPI_Wait(i8* %req, i8* null)
      call i32 @MPI_Send(i8* null, i32 1, i32 0, i32 0, i32 1, i8* %comm)
      ret i32 0
    }

    define i32 @main(i8* %comm) {
    entry:
      %a = call i32 @rank0(i8* %comm)
      %b = call i32 @rank1(i8* %comm)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  EXPECT_TRUE(analysis.getResults().potential_deadlocks.empty());
  EXPECT_FALSE(analysis.getRequestFacts().empty());
}

TEST_F(MPIAnalysisTest, CollectiveCountMismatchIsReportedPerCommunicatorClass) {
  const char *source = R"(
    declare i32 @MPI_Comm_dup(i8*, i8**)
    declare i32 @MPI_Bcast(i8*, i32, i32, i32, i8*)

    define i32 @rank0(i8* %comm) {
    entry:
      %dup = alloca i8*, align 8
      call i32 @MPI_Comm_dup(i8* %comm, i8** %dup)
      %dup_loaded = load i8*, i8** %dup, align 8
      call i32 @MPI_Bcast(i8* null, i32 1, i32 0, i32 0, i8* %comm)
      call i32 @MPI_Bcast(i8* null, i32 1, i32 0, i32 0, i8* %dup_loaded)
      ret i32 0
    }

    define i32 @rank1(i8* %comm) {
    entry:
      %dup = alloca i8*, align 8
      call i32 @MPI_Comm_dup(i8* %comm, i8** %dup)
      %dup_loaded = load i8*, i8** %dup, align 8
      call i32 @MPI_Bcast(i8* null, i32 1, i32 0, i32 0, i8* %comm)
      call i32 @MPI_Bcast(i8* null, i32 2, i32 0, i32 0, i8* %dup_loaded)
      ret i32 0
    }

    define i32 @main(i8* %comm) {
    entry:
      %a = call i32 @rank0(i8* %comm)
      %b = call i32 @rank1(i8* %comm)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  EXPECT_EQ(analysis.getResults().mismatched_collectives.size(), 1u);
}

TEST_F(MPIAnalysisTest, PopulatesAdditionalMPIResultBuckets) {
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

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  EXPECT_EQ(analysis.getResults().type_size_mismatches.size(), 2u);
  EXPECT_EQ(analysis.getResults().request_free_after_wait.size(), 1u);
  EXPECT_EQ(analysis.getResults().negative_root.size(), 1u);
  EXPECT_EQ(analysis.getResults().in_place_wrong_op.size(), 1u);
  EXPECT_EQ(analysis.getResults().destroy_null_comm.size(), 1u);
}

TEST_F(MPIAnalysisTest, WildcardsAreNotReportedAsInvalidRanksOrOutOfBounds) {
  const char *source = R"(
    declare i32 @MPI_Recv(i8*, i32, i32, i32, i32, i8*, i8*)

    define i32 @main(i8* %comm) {
    entry:
      call i32 @MPI_Recv(i8* null, i32 1, i32 0, i32 -1, i32 7, i8* %comm, i8* null)
      call i32 @MPI_Recv(i8* null, i32 1, i32 0, i32 -2, i32 7, i8* %comm, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  EXPECT_TRUE(analysis.getResults().invalid_ranks.empty());
  EXPECT_TRUE(analysis.getResults().rank_out_of_bounds.empty());
}

TEST_F(MPIAnalysisTest, InvalidNegativeRankIsReported) {
  const char *source = R"(
    declare i32 @MPI_Send(i8*, i32, i32, i32, i32, i8*)

    define i32 @main(i8* %comm) {
    entry:
      call i32 @MPI_Send(i8* null, i32 1, i32 0, i32 -3, i32 7, i8* %comm)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  EXPECT_EQ(analysis.getResults().invalid_ranks.size(), 1u);
  EXPECT_EQ(analysis.getResults().rank_out_of_bounds.size(), 1u);
}

TEST_F(MPIAnalysisTest, InvalidSendAnySourceRankIsReported) {
  const char *source = R"(
    declare i32 @MPI_Send(i8*, i32, i32, i32, i32, i8*)

    define i32 @main(i8* %comm) {
    entry:
      call i32 @MPI_Send(i8* null, i32 1, i32 0, i32 -1, i32 7, i8* %comm)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  EXPECT_EQ(analysis.getResults().invalid_ranks.size(), 1u);
  EXPECT_EQ(analysis.getResults().rank_out_of_bounds.size(), 1u);
}

TEST_F(MPIAnalysisTest, RankBeyondKnownCommunicatorBoundIsReported) {
  const char *source = R"(
    declare i32 @MPI_Send(i8*, i32, i32, i32, i32, i8*)

    define i32 @main(i8* %comm) {
    entry:
      call i32 @MPI_Send(i8* null, i32 1, i32 0, i32 2048, i32 7, i8* %comm)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  EXPECT_EQ(analysis.getResults().invalid_ranks.size(), 1u);
  EXPECT_EQ(analysis.getResults().rank_out_of_bounds.size(), 1u);
}

TEST_F(MPIAnalysisTest, RootBeyondKnownCommunicatorBoundIsReported) {
  const char *source = R"(
    declare i32 @MPI_Bcast(i8*, i32, i32, i32, i8*)

    define i32 @main(i8* %comm) {
    entry:
      call i32 @MPI_Bcast(i8* null, i32 1, i32 0, i32 2048, i8* %comm)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  EXPECT_EQ(analysis.getResults().negative_root.size(), 1u);
}

TEST_F(MPIAnalysisTest, SymbolicRankRangeAllowsMayMatchClassification) {
  const char *source = R"(
    declare i32 @MPI_Comm_rank(i8*, i32*)
    declare i32 @MPI_Send(i8*, i32, i32, i32, i32, i8*)
    declare i32 @MPI_Recv(i8*, i32, i32, i32, i32, i8*, i8*)

    define i32 @main(i8* %comm) {
    entry:
      %rank = alloca i32, align 4
      call i32 @MPI_Comm_rank(i8* %comm, i32* %rank)
      %loaded = load i32, i32* %rank, align 4
      %peer = add i32 %loaded, 1
      call i32 @MPI_Send(i8* null, i32 1, i32 0, i32 %peer, i32 7, i8* %comm)
      call i32 @MPI_Recv(i8* null, i32 1, i32 0, i32 2, i32 7, i8* %comm, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  auto sends =
      analysis.getProcessModel().getOperationsByKind(MPIOpKind::SEND_BLOCKING);
  auto recvs =
      analysis.getProcessModel().getOperationsByKind(MPIOpKind::RECV_BLOCKING);
  ASSERT_EQ(sends.size(), 1u);
  ASSERT_EQ(recvs.size(), 1u);
  EXPECT_EQ(analysis.getProcessModel().classifyCommunicationMatch(
                sends.front(), recvs.front()),
            MPICommunicationMatch::MayMatch);
}

TEST_F(MPIAnalysisTest, CancelWithoutWaitIsReportedAcrossControlFlow) {
  const char *source = R"(
    declare i32 @MPI_Isend(i8*, i32, i32, i32, i32, i8*, i8*)
    declare i32 @MPI_Cancel(i8*)

    define i32 @main(i8* %comm) {
    entry:
      %req = alloca i8, align 1
      call i32 @MPI_Isend(i8* null, i32 1, i32 0, i32 1, i32 7, i8* %comm, i8* %req)
      call i32 @MPI_Cancel(i8* %req)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  EXPECT_EQ(analysis.getResults().cancel_without_wait.size(), 1u);
}
