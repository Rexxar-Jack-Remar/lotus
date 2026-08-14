TEST_F(MPIAnalysisTest, CollectivesComparedPerCommunicatorAndSequenceSlot) {
  const char *source = R"(
    declare i32 @MPI_Bcast(i8*, i32, i32, i32, i8*)
    declare i32 @MPI_Reduce(i8*, i8*, i32, i32, i32, i32, i8*)
    declare i32 @MPI_Gather(i8*, i32, i32, i8*, i32, i32, i32, i8*)

    define i32 @main(i8* %comm_a, i8* %comm_b) {
    entry:
      call i32 @MPI_Bcast(i8* null, i32 1, i32 0, i32 0, i8* %comm_a)
      call i32 @MPI_Reduce(i8* null, i8* null, i32 1, i32 0, i32 0, i32 0, i8* %comm_b)
      call i32 @MPI_Gather(i8* null, i32 1, i32 0, i8* null, i32 1, i32 0, i32 0, i8* %comm_a)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  EXPECT_TRUE(analysis.getResults().mismatched_collectives.empty());
}

TEST_F(MPIAnalysisTest,
       CommunicatorDupReusesCanonicalIdentityWithoutFalseMismatch) {
  const char *source = R"(
    declare i32 @MPI_Comm_dup(i8*, i8**)
    declare i32 @MPI_Bcast(i8*, i32, i32, i32, i8*)
    declare i32 @MPI_Gather(i8*, i32, i32, i8*, i32, i32, i32, i8*)

    define i32 @main(i8* %comm) {
    entry:
      %dup = alloca i8*, align 8
      call i32 @MPI_Comm_dup(i8* %comm, i8** %dup)
      %dup_loaded = load i8*, i8** %dup, align 8
      call i32 @MPI_Bcast(i8* null, i32 1, i32 0, i32 0, i8* %comm)
      call i32 @MPI_Gather(i8* null, i32 1, i32 0, i8* null, i32 1, i32 0, i32 0, i8* %dup_loaded)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  EXPECT_TRUE(analysis.getResults().mismatched_collectives.empty());
}

TEST_F(MPIAnalysisTest, UnknownDistinctCommunicatorsDoNotForceDeadlockProof) {
  const char *source = R"(
    declare i32 @MPI_Send(i8*, i32, i32, i32, i32, i8*)
    declare i32 @MPI_Recv(i8*, i32, i32, i32, i32, i8*, i8*)

    define i32 @rank0(i1 %cond, i8* %comm_a, i8* %comm_b) {
    entry:
      %comm = select i1 %cond, i8* %comm_a, i8* %comm_b
      call i32 @MPI_Send(i8* null, i32 1, i32 0, i32 1, i32 9, i8* %comm)
      call i32 @MPI_Recv(i8* null, i32 1, i32 0, i32 1, i32 7, i8* %comm, i8* null)
      ret i32 0
    }

    define i32 @rank1(i1 %cond, i8* %comm_a, i8* %comm_b) {
    entry:
      %comm = select i1 %cond, i8* %comm_a, i8* %comm_b
      call i32 @MPI_Send(i8* null, i32 1, i32 0, i32 0, i32 7, i8* %comm)
      call i32 @MPI_Recv(i8* null, i32 1, i32 0, i32 0, i32 9, i8* %comm, i8* null)
      ret i32 0
    }

    define i32 @main(i1 %cond, i8* %comm_a, i8* %comm_b) {
    entry:
      %a = call i32 @rank0(i1 %cond, i8* %comm_a, i8* %comm_b)
      %b = call i32 @rank1(i1 %cond, i8* %comm_a, i8* %comm_b)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  EXPECT_TRUE(analysis.getResults().potential_deadlocks.empty());
}

TEST_F(MPIAnalysisTest,
       AmbiguousPointToPointCommunicatorDoesNotDisproveCommunication) {
  const char *source = R"(
    declare i32 @MPI_Send(i8*, i32, i32, i32, i32, i8*)
    declare i32 @MPI_Recv(i8*, i32, i32, i32, i32, i8*, i8*)

    define i32 @rank0(i1 %cond, i8* %comm_a, i8* %comm_b) {
    entry:
      %comm = select i1 %cond, i8* %comm_a, i8* %comm_b
      call i32 @MPI_Send(i8* null, i32 1, i32 0, i32 1, i32 9, i8* %comm)
      ret i32 0
    }

    define i32 @rank1(i1 %cond, i8* %comm_a, i8* %comm_b) {
    entry:
      %comm = select i1 %cond, i8* %comm_a, i8* %comm_b
      call i32 @MPI_Recv(i8* null, i32 1, i32 0, i32 0, i32 9, i8* %comm, i8* null)
      ret i32 0
    }

    define i32 @main(i1 %cond, i8* %comm_a, i8* %comm_b) {
    entry:
      %a = call i32 @rank0(i1 %cond, i8* %comm_a, i8* %comm_b)
      %b = call i32 @rank1(i1 %cond, i8* %comm_a, i8* %comm_b)
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

  EXPECT_NE(analysis.getProcessModel().classifyCommunicationMatch(
                sends.front(), recvs.front()),
            MPICommunicationMatch::NoMatch);
}

TEST_F(MPIAnalysisTest,
       AmbiguousPointToPointCommunicatorEmitsModelGapAndKeepsChannel) {
  const char *source = R"(
    declare i32 @MPI_Send(i8*, i32, i32, i32, i32, i8*)
    declare i32 @MPI_Recv(i8*, i32, i32, i32, i32, i8*, i8*)

    define i32 @helper_send(i1 %cond, i8* %comm_a, i8* %comm_b) {
    entry:
      %comm = select i1 %cond, i8* %comm_a, i8* %comm_b
      call i32 @MPI_Send(i8* null, i32 1, i32 0, i32 1, i32 5, i8* %comm)
      ret i32 0
    }

    define i32 @helper_recv(i1 %cond, i8* %comm_a, i8* %comm_b) {
    entry:
      %comm = select i1 %cond, i8* %comm_a, i8* %comm_b
      call i32 @MPI_Recv(i8* null, i32 1, i32 0, i32 0, i32 5, i8* %comm, i8* null)
      ret i32 0
    }

    define i32 @main(i1 %cond, i8* %comm_a, i8* %comm_b) {
    entry:
      %a = call i32 @helper_send(i1 %cond, i8* %comm_a, i8* %comm_b)
      %b = call i32 @helper_recv(i1 %cond, i8* %comm_a, i8* %comm_b)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  bool saw_model_gap = false;
  for (const auto &gap : analysis.getResults().model_gaps) {
    if (gap.code == "mpi_channel_identity_unresolved") {
      saw_model_gap = true;
      break;
    }
  }
  EXPECT_TRUE(saw_model_gap);

  bool saw_channel_relation = false;
  for (const auto &diag : analysis.getResults().diagnostics) {
    if (diag.code == "mpi_channel_identity_unresolved") {
      saw_channel_relation = true;
      break;
    }
  }
  EXPECT_TRUE(saw_channel_relation);
}

TEST_F(MPIAnalysisTest, CollectiveMatchingUsesPerCommunicatorSequenceSlots) {
  const char *source = R"(
    declare i32 @MPI_Bcast(i8*, i32, i32, i32, i8*)
    declare i32 @MPI_Gather(i8*, i32, i32, i8*, i32, i32, i32, i8*)
    declare i32 @MPI_Reduce(i8*, i8*, i32, i32, i32, i32, i8*)

    define i32 @rank0(i8* %comm) {
    entry:
      call i32 @MPI_Bcast(i8* null, i32 1, i32 0, i32 0, i8* %comm)
      call i32 @MPI_Gather(i8* null, i32 1, i32 0, i8* null, i32 1, i32 0, i32 0, i8* %comm)
      ret i32 0
    }

    define i32 @rank1(i8* %comm) {
    entry:
      call i32 @MPI_Bcast(i8* null, i32 1, i32 0, i32 0, i8* %comm)
      call i32 @MPI_Reduce(i8* null, i8* null, i32 1, i32 0, i32 0, i32 0, i8* %comm)
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

TEST_F(MPIAnalysisTest, CollectiveProtocolAutomatonTracksParticipantSlots) {
  const char *source = R"(
    declare i32 @MPI_Bcast(i8*, i32, i32, i32, i8*)
    declare i32 @MPI_Gather(i8*, i32, i32, i8*, i32, i32, i32, i8*)
    declare i32 @MPI_Reduce(i8*, i8*, i32, i32, i32, i32, i8*)

    define i32 @rank0(i8* %comm) {
    entry:
      call i32 @MPI_Bcast(i8* null, i32 1, i32 0, i32 0, i8* %comm)
      call i32 @MPI_Gather(i8* null, i32 1, i32 0, i8* null, i32 1, i32 0, i32 0, i8* %comm)
      ret i32 0
    }

    define i32 @rank1(i8* %comm) {
    entry:
      call i32 @MPI_Bcast(i8* null, i32 1, i32 0, i32 0, i8* %comm)
      call i32 @MPI_Reduce(i8* null, i8* null, i32 1, i32 0, i32 0, i32 0, i8* %comm)
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

  const auto &automata = analysis.getCollectiveAnalysis().getProtocolAutomata();
  ASSERT_EQ(automata.size(), 1u);
  const auto &automaton = automata.begin()->second;
  EXPECT_EQ(automaton.participant_slots.size(), 1u);
  EXPECT_EQ(automaton.slots.size(), 4u);
  EXPECT_EQ(automaton.slots.at(0).expected_type, ThreadAPI::TD_MPI_BCAST);
}

TEST_F(MPIAnalysisTest, RankGuardedCollectiveIsReportedConditional) {
  const char *source = R"(
    declare i32 @MPI_Comm_rank(i8*, i32*)
    declare i32 @MPI_Bcast(i8*, i32, i32, i32, i8*)

    define i32 @main(i8* %comm) {
    entry:
      %rank = alloca i32, align 4
      call i32 @MPI_Comm_rank(i8* %comm, i32* %rank)
      %loaded = load i32, i32* %rank, align 4
      %is_root = icmp eq i32 %loaded, 0
      br i1 %is_root, label %then, label %done

    then:
      call i32 @MPI_Bcast(i8* null, i32 1, i32 0, i32 0, i8* %comm)
      br label %done

    done:
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  EXPECT_EQ(analysis.getResults().conditional_collectives.size(), 1u);
  const auto &diagnostics =
      analysis.getCollectiveAnalysis().getProtocolDiagnostics();
  auto it = diagnostics.find("collective_rank_filtered");
  ASSERT_NE(it, diagnostics.end());
  EXPECT_GT(it->second, 0u);
}
