TEST_F(MPIAnalysisTest, RankRestrictedCollectivesUseDistinctProtocolSlots) {
  const char *source = R"(
    declare i32 @MPI_Comm_rank(i8*, i32*)
    declare i32 @MPI_Bcast(i8*, i32, i32, i32, i8*)

    define i32 @main(i8* %comm) {
    entry:
      %rank = alloca i32, align 4
      call i32 @MPI_Comm_rank(i8* %comm, i32* %rank)
      %rankv = load i32, i32* %rank, align 4
      %in_low_half = icmp slt i32 %rankv, 2
      br i1 %in_low_half, label %low, label %high

    low:
      call i32 @MPI_Bcast(i8* null, i32 1, i32 0, i32 0, i8* %comm)
      br label %join

    high:
      call i32 @MPI_Bcast(i8* null, i32 1, i32 0, i32 0, i8* %comm)
      br label %join

    join:
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  auto collectives = analysis.getProcessModel().getOperationsByKind(
      MPIOpKind::COLLECTIVE_BLOCKING);
  ASSERT_EQ(collectives.size(), 2u);
  EXPECT_EQ(collectives[0].communicator_subgroup_id, 0u);
  EXPECT_EQ(collectives[1].communicator_subgroup_id, 0u);
  EXPECT_NE(collectives[0].participant_class_id, 0u);
  EXPECT_NE(collectives[1].participant_class_id, 0u);
  EXPECT_NE(collectives[0].participant_class_id,
            collectives[1].participant_class_id);
  EXPECT_EQ(collectives[0].protocol_sequence_id, 0u);
  EXPECT_EQ(collectives[1].protocol_sequence_id, 1u);
}

TEST_F(MPIAnalysisTest, SameFunctionRankPartitionedPointToPointStillMatches) {
  const char *source = R"(
    declare i32 @MPI_Comm_rank(i8*, i32*)
    declare i32 @MPI_Send(i8*, i32, i32, i32, i32, i8*)
    declare i32 @MPI_Recv(i8*, i32, i32, i32, i32, i8*, i8*)

    define i32 @main(i8* %comm) {
    entry:
      %rank = alloca i32, align 4
      call i32 @MPI_Comm_rank(i8* %comm, i32* %rank)
      %rankv = load i32, i32* %rank, align 4
      %is_zero = icmp eq i32 %rankv, 0
      br i1 %is_zero, label %send, label %check_recv

    send:
      call i32 @MPI_Send(i8* null, i32 1, i32 0, i32 1, i32 7, i8* %comm)
      br label %join

    check_recv:
      %is_one = icmp eq i32 %rankv, 1
      br i1 %is_one, label %recv, label %join

    recv:
      call i32 @MPI_Recv(i8* null, i32 1, i32 0, i32 0, i32 7, i8* %comm, i8* null)
      br label %join

    join:
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
  EXPECT_TRUE(
      analysis.getProcessModel().canCommunicate(sends.front(), recvs.front()));
  EXPECT_NE(analysis.getProcessModel().classifyCommunicationMatch(
                sends.front(), recvs.front()),
            MPICommunicationMatch::NoMatch);
  EXPECT_FALSE(analysis.getResults().channel_obligations.empty());
}

TEST_F(MPIAnalysisTest, FlushMarksTrackedRMACompletion) {
  const char *source = R"(
    declare i32 @MPI_Win_create(i8*, i64, i32, i8*, i8*)
    declare i32 @MPI_Win_lock(i32, i32, i32, i8*)
    declare i32 @MPI_Get(i8*, i32, i32, i32, i64, i32, i32, i8*)
    declare i32 @MPI_Win_flush(i32, i8*)
    declare i32 @MPI_Win_unlock(i32, i8*)
    @win = global i8 0, align 1

    define i32 @main(i8* %comm) {
    entry:
      call i32 @MPI_Win_create(i8* null, i64 8, i32 4, i8* %comm, i8* @win)
      call i32 @MPI_Win_lock(i32 0, i32 1, i32 0, i8* @win)
      call i32 @MPI_Get(i8* null, i32 1, i32 0, i32 1, i64 0, i32 1, i32 0, i8* @win)
      call i32 @MPI_Win_flush(i32 1, i8* @win)
      call i32 @MPI_Win_unlock(i32 1, i8* @win)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  EXPECT_TRUE(analysis.getResults().unsynchronized_rma.empty());
  EXPECT_FALSE(analysis.getResults().rma_synchronization_facts.empty());
  EXPECT_TRUE(analysis.getResults().rma_races.empty());
}

TEST_F(MPIAnalysisTest, WaitsomeWithoutRecoverableIndicesKeepsRequestsPending) {
  const char *source = R"(
    declare i32 @MPI_Isend(i8*, i32, i32, i32, i32, i8*, i8*)
    declare i32 @MPI_Waitsome(i32, i8**, i32*, i32*, i8*)

    define i32 @main(i8* %comm) {
    entry:
      %req1 = alloca i8, align 1
      %req2 = alloca i8, align 1
      %reqs = alloca [2 x i8*], align 8
      %slot0 = getelementptr inbounds [2 x i8*], [2 x i8*]* %reqs, i64 0, i64 0
      %slot1 = getelementptr inbounds [2 x i8*], [2 x i8*]* %reqs, i64 0, i64 1
      %indices = alloca [2 x i32], align 4
      %idx0 = getelementptr inbounds [2 x i32], [2 x i32]* %indices, i64 0, i64 0

      store i8* %req1, i8** %slot0, align 8
      store i8* %req2, i8** %slot1, align 8

      call i32 @MPI_Isend(i8* null, i32 1, i32 0, i32 1, i32 7, i8* %comm, i8* %req1)
      call i32 @MPI_Isend(i8* null, i32 1, i32 0, i32 2, i32 8, i8* %comm, i8* %req2)
      call i32 @MPI_Waitsome(i32 2, i8** %slot0, i32* null, i32* %idx0, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  EXPECT_TRUE(analysis.getResults().orphaned_requests.empty());
  size_t may_complete = 0;
  for (const auto &op : analysis.getProcessModel().getAllOperations()) {
    if (op.request_state == RequestCompletionState::MayComplete) {
      ++may_complete;
    }
  }
  EXPECT_EQ(may_complete, 2u);
}

TEST_F(MPIAnalysisTest, WaitsomeCompletesOnlyRecoveredIndices) {
  const char *source = R"(
    declare i32 @MPI_Isend(i8*, i32, i32, i32, i32, i8*, i8*)
    declare i32 @MPI_Waitsome(i32, i8**, i32*, i32*, i8*)

    define i32 @main(i8* %comm) {
    entry:
      %req1 = alloca i8, align 1
      %req2 = alloca i8, align 1
      %reqs = alloca [2 x i8*], align 8
      %slot0 = getelementptr inbounds [2 x i8*], [2 x i8*]* %reqs, i64 0, i64 0
      %slot1 = getelementptr inbounds [2 x i8*], [2 x i8*]* %reqs, i64 0, i64 1
      %indices = alloca [1 x i32], align 4
      %idx0 = getelementptr inbounds [1 x i32], [1 x i32]* %indices, i64 0, i64 0

      store i8* %req1, i8** %slot0, align 8
      store i8* %req2, i8** %slot1, align 8
      store i32 1, i32* %idx0, align 4

      call i32 @MPI_Isend(i8* null, i32 1, i32 0, i32 1, i32 7, i8* %comm, i8* %req1)
      call i32 @MPI_Isend(i8* null, i32 1, i32 0, i32 2, i32 8, i8* %comm, i8* %req2)
      call i32 @MPI_Waitsome(i32 2, i8** %slot0, i32* null, i32* %idx0, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  EXPECT_EQ(analysis.getResults().orphaned_requests.size(), 1u);
}

TEST_F(MPIAnalysisTest, SendrecvExtractionUsesCorrectOperands) {
  const char *source = R"(
    declare i32 @MPI_Sendrecv(i8*, i32, i32, i32, i32,
                              i8*, i32, i32, i32, i32, i8*, i8*)

    define i32 @main(i8* %comm) {
    entry:
      call i32 @MPI_Sendrecv(i8* null, i32 1, i32 0, i32 7, i32 11,
                             i8* null, i32 1, i32 0, i32 3, i32 13, i8* %comm, i8* null)
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
  EXPECT_EQ(sends.front().dest_rank, 7);
  EXPECT_EQ(sends.front().tag, 11);
  EXPECT_EQ(recvs.front().source_rank, 3);
  EXPECT_EQ(recvs.front().tag, 13);
  EXPECT_EQ(sends.front().communicator, recvs.front().communicator);
}

TEST_F(MPIAnalysisTest, WildcardSourceAndTagSupportMinusTwoSentinel) {
  const char *source = R"(
    declare i32 @MPI_Send(i8*, i32, i32, i32, i32, i8*)
    declare i32 @MPI_Recv(i8*, i32, i32, i32, i32, i8*, i8*)

    define i32 @main(i8* %comm) {
    entry:
      call i32 @MPI_Send(i8* null, i32 1, i32 0, i32 3, i32 7, i8* %comm)
      call i32 @MPI_Recv(i8* null, i32 1, i32 0, i32 -2, i32 -2, i8* %comm, i8* null)
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
  EXPECT_TRUE(
      analysis.getProcessModel().canCommunicate(sends.front(), recvs.front()));
  EXPECT_EQ(analysis.getProcessModel().classifyCommunicationMatch(
                sends.front(), recvs.front()),
            MPICommunicationMatch::MayMatch);
}
