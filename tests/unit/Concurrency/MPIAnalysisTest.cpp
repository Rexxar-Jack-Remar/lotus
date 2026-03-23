#include "Analysis/Concurrency/MPI/MPIAnalysis.h"

#include "Analysis/Concurrency/MPI/MPISemantics.h"

#include "TestUtils/LLVMHelpers.h"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <vector>

using namespace llvm;
using namespace mpi;

class MPIAnalysisTest : public lotus::unittest::LlvmModuleTest {};

static const MPIOperation *findOperation(const std::vector<MPIOperation> &ops,
                                         ThreadAPI::TD_TYPE type) {
  for (const auto &op : ops) {
    if (op.td_type == type) {
      return &op;
    }
  }
  return nullptr;
}

TEST_F(MPIAnalysisTest, SendRecvCreatesSendAndReceiveOperations) {
  const char *source = R"(
    declare i32 @MPI_Sendrecv(i8*, i32, i32, i32, i32,
                              i8*, i32, i32, i32, i32, i8*, i8*)

    define i32 @main(i8* %comm) {
    entry:
      call i32 @MPI_Sendrecv(i8* null, i32 1, i32 0, i32 1, i32 7,
                             i8* null, i32 1, i32 0, i32 0, i32 7, i8* %comm, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  EXPECT_EQ(analysis.getProcessModel()
                .getOperationsByKind(MPIOpKind::SEND_BLOCKING)
                .size(),
            1u);
  EXPECT_EQ(analysis.getProcessModel()
                .getOperationsByKind(MPIOpKind::RECV_BLOCKING)
                .size(),
            1u);
}

TEST_F(MPIAnalysisTest, RankIncompatiblePointToPointDoesNotMatch) {
  const char *source = R"(
    declare i32 @MPI_Comm_rank(i8*, i32*)
    declare i32 @MPI_Send(i8*, i32, i32, i32, i32, i8*)
    declare i32 @MPI_Recv(i8*, i32, i32, i32, i32, i8*, i8*)

    define i32 @main(i8* %comm) {
    entry:
      %rank = alloca i32, align 4
      call i32 @MPI_Comm_rank(i8* %comm, i32* %rank)
      %rankv = load i32, i32* %rank, align 4
      %is_one = icmp eq i32 %rankv, 1
      br i1 %is_one, label %send, label %check_recv

    send:
      call i32 @MPI_Send(i8* null, i32 1, i32 0, i32 0, i32 7, i8* %comm)
      br label %join

    check_recv:
      %is_zero = icmp eq i32 %rankv, 0
      br i1 %is_zero, label %recv, label %join

    recv:
      call i32 @MPI_Recv(i8* null, i32 1, i32 0, i32 2, i32 7, i8* %comm, i8* null)
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
  EXPECT_FALSE(
      analysis.getProcessModel().canCommunicate(sends.front(), recvs.front()));
  EXPECT_EQ(analysis.getProcessModel().classifyCommunicationMatch(
                sends.front(), recvs.front()),
            MPICommunicationMatch::NoMatch);
}

TEST_F(MPIAnalysisTest, WaitAllCompletesOnlyListedRequests) {
  const char *source = R"(
    declare i32 @MPI_Isend(i8*, i32, i32, i32, i32, i8*, i8*)
    declare i32 @MPI_Waitall(i32, i8**, i8*)

    define i32 @main(i8* %comm) {
    entry:
      %req1 = alloca i8, align 1
      %req2 = alloca i8, align 1
      %reqs = alloca [1 x i8*], align 8
      %slot0 = getelementptr inbounds [1 x i8*], [1 x i8*]* %reqs, i64 0, i64 0
      store i8* %req1, i8** %slot0, align 8
      call i32 @MPI_Isend(i8* null, i32 1, i32 0, i32 1, i32 7, i8* %comm, i8* %req1)
      call i32 @MPI_Isend(i8* null, i32 1, i32 0, i32 2, i32 8, i8* %comm, i8* %req2)
      call i32 @MPI_Waitall(i32 1, i8** %slot0, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  EXPECT_EQ(analysis.getResults().orphaned_requests.size(), 1u);
}

TEST_F(MPIAnalysisTest, IprobeDoesNotEnterRequestLifecycle) {
  const char *source = R"(
    declare i32 @MPI_Isend(i8*, i32, i32, i32, i32, i8*, i8*)
    declare i32 @MPI_Iprobe(i32, i32, i8*, i32*, i8*)

    define i32 @main(i8* %comm) {
    entry:
      %req = alloca i8, align 1
      %flag = alloca i32, align 4
      %status = alloca i8, align 1
      call i32 @MPI_Isend(i8* null, i32 1, i32 0, i32 1, i32 7, i8* %comm, i8* %req)
      call i32 @MPI_Iprobe(i32 1, i32 7, i8* %comm, i32* %flag, i8* %status)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  EXPECT_EQ(analysis.getResults().orphaned_requests.size(), 1u);
}

TEST_F(MPIAnalysisTest, MatchedMessageOperationsUseReceiveSemantics) {
  const char *source = R"(
    declare i32 @MPI_Mprobe(i32, i32, i8*, i8*, i8*)
    declare i32 @MPI_Improbe(i32, i32, i8*, i32*, i8*, i8*)
    declare i32 @MPI_Mrecv(i8*, i32, i32, i8*, i8*)
    declare i32 @MPI_Imrecv(i8*, i32, i32, i8*, i8*)

    define i32 @main(i8* %comm) {
    entry:
      %flag = alloca i32, align 4
      %msg1 = alloca i8, align 1
      %msg2 = alloca i8, align 1
      %status = alloca i8, align 1
      %req = alloca i8, align 1
      call i32 @MPI_Mprobe(i32 1, i32 7, i8* %comm, i8* %msg1, i8* %status)
      call i32 @MPI_Improbe(i32 1, i32 7, i8* %comm, i32* %flag, i8* %msg2, i8* %status)
      call i32 @MPI_Mrecv(i8* null, i32 4, i32 0, i8* %msg1, i8* %status)
      call i32 @MPI_Imrecv(i8* null, i32 4, i32 0, i8* %msg2, i8* %req)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  const auto &ops = analysis.getProcessModel().getAllOperations();
  auto findOp = [&](auto predicate) -> const MPIOperation * {
    for (const auto &op : ops) {
      if (predicate(op)) {
        return &op;
      }
    }
    return nullptr;
  };
  const MPIOperation *mprobe = findOp([](const MPIOperation &op) {
    return op.kind == MPIOpKind::PROBE_BLOCKING;
  });
  const MPIOperation *improbe = findOp([](const MPIOperation &op) {
    return op.td_type == ThreadAPI::TD_MPI_IMPROBE;
  });
  const MPIOperation *mrecv = findOp([](const MPIOperation &op) {
    return op.td_type == ThreadAPI::TD_MPI_MRECV;
  });
  const MPIOperation *imrecv = findOp([](const MPIOperation &op) {
    return op.td_type == ThreadAPI::TD_MPI_IMRECV;
  });
  ASSERT_NE(mprobe, nullptr);
  ASSERT_NE(improbe, nullptr);
  ASSERT_NE(mrecv, nullptr);
  ASSERT_NE(imrecv, nullptr);

  EXPECT_EQ(mprobe->kind, MPIOpKind::PROBE_BLOCKING);
  EXPECT_EQ(mprobe->blocking_mode, MPIBlockingMode::Blocking);
  EXPECT_EQ(improbe->kind, MPIOpKind::PROBE_NONBLOCKING);
  EXPECT_EQ(improbe->blocking_mode, MPIBlockingMode::NonBlocking);

  EXPECT_EQ(mrecv->kind, MPIOpKind::RECV_BLOCKING);
  EXPECT_EQ(mrecv->blocking_mode, MPIBlockingMode::Blocking);
  EXPECT_TRUE(mrecv->matched_message);
  EXPECT_EQ(imrecv->kind, MPIOpKind::RECV_NONBLOCKING);
  EXPECT_EQ(imrecv->blocking_mode, MPIBlockingMode::NonBlocking);
  EXPECT_TRUE(imrecv->matched_message);
  EXPECT_NE(imrecv->request, nullptr);
}

TEST_F(MPIAnalysisTest, BitcastedMPICallStillLowersIntoOperation) {
  const char *source = R"(
    declare i32 @MPI_Barrier(i8*)

    define i32 @main(i8* %comm) {
    entry:
      %bar = call i32 bitcast (i32 (i8*)* @MPI_Barrier to i32 (i8*)*)(i8* %comm)
      ret i32 %bar
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  EXPECT_EQ(analysis.getOperationCount(MPIOpKind::BARRIER_BLOCKING), 1u);
}

TEST_F(MPIAnalysisTest, ReceiveAnyTagIsNotReportedAsInvalidTag) {
  const char *source = R"(
    declare i32 @MPI_Recv(i8*, i32, i32, i32, i32, i8*, i8*)

    define i32 @main(i8* %comm) {
    entry:
      %status = alloca i8, align 1
      %recv = call i32 @MPI_Recv(i8* null, i32 1, i32 0, i32 1, i32 -1,
                                 i8* %comm, i8* %status)
      ret i32 %recv
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  EXPECT_TRUE(analysis.getResults().invalid_tags.empty());
}

TEST_F(MPIAnalysisTest, TestWithFalseFlagKeepsRequestPending) {
  const char *source = R"(
    declare i32 @MPI_Isend(i8*, i32, i32, i32, i32, i8*, i8*)
    declare i32 @MPI_Test(i8*, i32*, i8*)

    define i32 @main(i8* %comm) {
    entry:
      %req = alloca i8, align 1
      %flag = alloca i32, align 4
      store i32 0, i32* %flag, align 4
      call i32 @MPI_Isend(i8* null, i32 1, i32 0, i32 1, i32 7, i8* %comm, i8* %req)
      call i32 @MPI_Test(i8* %req, i32* %flag, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  EXPECT_EQ(analysis.getResults().orphaned_requests.size(), 1u);
}

TEST_F(MPIAnalysisTest, TestanyWithoutRecoverableIndexDoesNotCompleteRequest) {
  const char *source = R"(
    declare i32 @MPI_Isend(i8*, i32, i32, i32, i32, i8*, i8*)
    declare i32 @MPI_Testany(i32, i8**, i32*, i32*, i8*)

    define i32 @main(i8* %comm) {
    entry:
      %req1 = alloca i8, align 1
      %req2 = alloca i8, align 1
      %reqs = alloca [2 x i8*], align 8
      %slot0 = getelementptr inbounds [2 x i8*], [2 x i8*]* %reqs, i64 0, i64 0
      %slot1 = getelementptr inbounds [2 x i8*], [2 x i8*]* %reqs, i64 0, i64 1
      %index = alloca i32, align 4
      %flag = alloca i32, align 4
      store i8* %req1, i8** %slot0, align 8
      store i8* %req2, i8** %slot1, align 8
      store i32 1, i32* %flag, align 4
      call i32 @MPI_Isend(i8* null, i32 1, i32 0, i32 1, i32 7, i8* %comm, i8* %req1)
      call i32 @MPI_Isend(i8* null, i32 1, i32 0, i32 2, i32 8, i8* %comm, i8* %req2)
      call i32 @MPI_Testany(i32 2, i8** %slot0, i32* %index, i32* %flag, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  EXPECT_TRUE(analysis.getResults().orphaned_requests.empty());
  const auto &deferred = analysis.getProcessModel().getDeferredLoweringStats();
  auto it = deferred.find("testany_unknown_index");
  ASSERT_NE(it, deferred.end());
  EXPECT_GT(it->second, 0u);
  size_t may_complete = 0;
  for (const auto &op : analysis.getProcessModel().getAllOperations()) {
    if (op.request_state == RequestCompletionState::MayComplete) {
      ++may_complete;
    }
  }
  EXPECT_EQ(may_complete, 2u);
}

TEST_F(MPIAnalysisTest, WaitanyWithoutRecoverableIndexDoesNotCompleteRequest) {
  const char *source = R"(
    declare i32 @MPI_Isend(i8*, i32, i32, i32, i32, i8*, i8*)
    declare i32 @MPI_Waitany(i32, i8**, i32*, i8*)

    define i32 @main(i8* %comm) {
    entry:
      %req1 = alloca i8, align 1
      %req2 = alloca i8, align 1
      %reqs = alloca [2 x i8*], align 8
      %slot0 = getelementptr inbounds [2 x i8*], [2 x i8*]* %reqs, i64 0, i64 0
      %slot1 = getelementptr inbounds [2 x i8*], [2 x i8*]* %reqs, i64 0, i64 1
      %index = alloca i32, align 4
      store i8* %req1, i8** %slot0, align 8
      store i8* %req2, i8** %slot1, align 8
      call i32 @MPI_Isend(i8* null, i32 1, i32 0, i32 1, i32 7, i8* %comm, i8* %req1)
      call i32 @MPI_Isend(i8* null, i32 1, i32 0, i32 2, i32 8, i8* %comm, i8* %req2)
      call i32 @MPI_Waitany(i32 2, i8** %slot0, i32* %index, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  EXPECT_TRUE(analysis.getResults().orphaned_requests.empty());
  const auto &deferred = analysis.getProcessModel().getDeferredLoweringStats();
  auto it = deferred.find("waitany_unknown_index");
  ASSERT_NE(it, deferred.end());
  EXPECT_GT(it->second, 0u);
}

TEST_F(MPIAnalysisTest, NonBlockingCollectiveRequestCompletesThroughWait) {
  const char *source = R"(
    declare i32 @MPI_Ibcast(i8*, i32, i32, i32, i8*, i8*)
    declare i32 @MPI_Wait(i8*, i8*)

    define i32 @main(i8* %comm) {
    entry:
      %req = alloca i8, align 1
      call i32 @MPI_Ibcast(i8* null, i32 1, i32 0, i32 0, i8* %comm, i8* %req)
      call i32 @MPI_Wait(i8* %req, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  EXPECT_TRUE(analysis.getResults().orphaned_requests.empty());
}

TEST_F(MPIAnalysisTest, RequestFreeTerminatesOutstandingRequest) {
  const char *source = R"(
    declare i32 @MPI_Ibarrier(i8*, i8*)
    declare i32 @MPI_Request_free(i8*)

    define i32 @main(i8* %comm) {
    entry:
      %req = alloca i8, align 1
      call i32 @MPI_Ibarrier(i8* %comm, i8* %req)
      call i32 @MPI_Request_free(i8* %req)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  EXPECT_TRUE(analysis.getResults().orphaned_requests.empty());
}

TEST_F(MPIAnalysisTest, NonblockingCollectiveUsesSingleRequestArity) {
  const char *source = R"(
    declare i32 @MPI_Ibarrier(i8*, i8*)

    define i32 @main(i8* %comm) {
    entry:
      %req = alloca i8, align 1
      call i32 @MPI_Ibarrier(i8* %comm, i8* %req)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  const auto &request_sets = analysis.getRequestSetFacts();
  auto issued = std::find_if(
      request_sets.begin(), request_sets.end(),
      [](const MPIRequestSetFact &fact) {
        return fact.provenance == "mpi_request_set_issue";
      });
  ASSERT_NE(issued, request_sets.end());
  EXPECT_EQ(issued->arity, MPIRequestArity::Single);
  EXPECT_EQ(issued->kind, MPIRequestSetKind::Collective);
}

TEST_F(MPIAnalysisTest, SemanticEventsCaptureCollectiveAndRequestSemantics) {
  const char *source = R"(
    declare i32 @MPI_Isend(i8*, i32, i32, i32, i32, i8*, i8*)
    declare i32 @MPI_Wait(i8*, i8*)
    declare i32 @MPI_Bcast(i8*, i32, i32, i32, i8*)

    define i32 @main(i8* %comm) {
    entry:
      %req = alloca i8, align 1
      call i32 @MPI_Isend(i8* null, i32 1, i32 0, i32 1, i32 7, i8* %comm, i8* %req)
      call i32 @MPI_Wait(i8* %req, i8* null)
      call i32 @MPI_Bcast(i8* null, i32 1, i32 0, i32 0, i8* %comm)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  bool saw_request_issue = false;
  bool saw_request_completion = false;
  bool saw_collective = false;
  for (const auto &event : analysis.getProcessModel().getSemanticEvents()) {
    if (event.request.action == MPIRequestActionKind::IssueNonBlocking) {
      saw_request_issue = true;
    }
    if (event.request.action == MPIRequestActionKind::CompleteMust) {
      saw_request_completion = true;
    }
    if (event.has_collective_semantics &&
        event.collective.type == ThreadAPI::TD_MPI_BCAST &&
        event.collective.count == 1 && event.collective.root_rank == 0) {
      saw_collective = true;
    }
  }

  EXPECT_TRUE(saw_request_issue);
  EXPECT_TRUE(saw_request_completion);
  EXPECT_TRUE(saw_collective);
}

TEST_F(MPIAnalysisTest, SemanticEventsCapturePointToPointObligations) {
  const char *source = R"(
    declare i32 @MPI_Send(i8*, i32, i32, i32, i32, i8*)
    declare i32 @MPI_Recv(i8*, i32, i32, i32, i32, i8*, i8*)

    define i32 @main(i8* %comm) {
    entry:
      call i32 @MPI_Send(i8* null, i32 1, i32 0, i32 0, i32 7, i8* %comm)
      call i32 @MPI_Recv(i8* null, i32 1, i32 0, i32 0, i32 7, i8* %comm, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  bool saw_send_event = false;
  bool saw_recv_event = false;
  for (const auto &event : analysis.getProcessModel().getSemanticEvents()) {
    if (!event.has_point_to_point_semantics) {
      continue;
    }
    saw_send_event = saw_send_event || event.point_to_point.is_send;
    saw_recv_event = saw_recv_event || event.point_to_point.is_recv;
  }

  const auto &obligations =
      analysis.getProcessModel().getPointToPointObligations();
  ASSERT_EQ(obligations.size(), 1u);
  EXPECT_TRUE(saw_send_event);
  EXPECT_TRUE(saw_recv_event);
  EXPECT_EQ(obligations.front().proof, MPIMatchProofKind::MustMatch);
  EXPECT_EQ(obligations.front().relation.proof,
            concurrency::ProofStrength::Must);
}

TEST_F(MPIAnalysisTest, SemanticEventsCaptureRMAEpochFacts) {
  const char *source = R"(
    declare i32 @MPI_Win_create(i8*, i32, i32, i8*, i8*)
    declare i32 @MPI_Win_lock(i32, i32, i32, i8*)
    declare i32 @MPI_Put(i8*, i32, i32, i32, i32, i32, i32, i8*)
    declare i32 @MPI_Win_flush(i32, i8*)
    declare i32 @MPI_Win_unlock(i32, i8*)

    define i32 @main(i8* %comm) {
    entry:
      %win = alloca i8, align 1
      call i32 @MPI_Win_create(i8* null, i32 8, i32 4, i8* %comm, i8* %win)
      call i32 @MPI_Win_lock(i32 0, i32 1, i32 0, i8* %win)
      call i32 @MPI_Put(i8* null, i32 1, i32 0, i32 1, i32 0, i32 1, i32 0, i8* %win)
      call i32 @MPI_Win_flush(i32 1, i8* %win)
      call i32 @MPI_Win_unlock(i32 1, i8* %win)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  bool saw_epoch = false;
  bool saw_flush_completion = false;
  for (const auto &event : analysis.getProcessModel().getSemanticEvents()) {
    if (!event.has_rma_semantics || !event.rma.is_data_operation) {
      continue;
    }
    saw_epoch = event.rma.epoch_id != 0 &&
                event.rma.sync_model == MPIRMASyncModel::LockUnlock;
    saw_flush_completion = event.rma.epoch_completion ==
                           MPIRMAEpochCompletionKind::RemoteGuaranteed;
  }

  EXPECT_TRUE(saw_epoch);
  EXPECT_TRUE(saw_flush_completion);
}

TEST_F(MPIAnalysisTest, StartedPersistentRequestWithoutCompletionIsOrphaned) {
  const char *source = R"(
    declare i32 @MPI_Send_init(i8*, i32, i32, i32, i32, i8*, i8*)
    declare i32 @MPI_Start(i8*)

    define i32 @main(i8* %comm) {
    entry:
      %req = alloca i8, align 1
      call i32 @MPI_Send_init(i8* null, i32 1, i32 0, i32 1, i32 7, i8* %comm, i8* %req)
      call i32 @MPI_Start(i8* %req)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  ASSERT_EQ(analysis.getResults().orphaned_requests.size(), 1u);
  EXPECT_NE(analysis.getResults().orphaned_requests.front().issue_inst,
            nullptr);
}

TEST_F(MPIAnalysisTest, StartedPersistentRequestCompletesThroughWait) {
  const char *source = R"(
    declare i32 @MPI_Send_init(i8*, i32, i32, i32, i32, i8*, i8*)
    declare i32 @MPI_Start(i8*)
    declare i32 @MPI_Wait(i8*, i8*)

    define i32 @main(i8* %comm) {
    entry:
      %req = alloca i8, align 1
      call i32 @MPI_Send_init(i8* null, i32 1, i32 0, i32 1, i32 7, i8* %comm, i8* %req)
      call i32 @MPI_Start(i8* %req)
      call i32 @MPI_Wait(i8* %req, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  EXPECT_TRUE(analysis.getResults().orphaned_requests.empty());
}

TEST_F(MPIAnalysisTest, RequestStateDomainTracksPersistentLifecycleHistory) {
  const char *source = R"(
    declare i32 @MPI_Send_init(i8*, i32, i32, i32, i32, i8*, i8*)
    declare i32 @MPI_Start(i8*)
    declare i32 @MPI_Wait(i8*, i8*)
    declare i32 @MPI_Request_free(i8*)

    define i32 @main(i8* %comm) {
    entry:
      %req = alloca i8, align 1
      call i32 @MPI_Send_init(i8* null, i32 1, i32 0, i32 1, i32 7, i8* %comm, i8* %req)
      call i32 @MPI_Start(i8* %req)
      call i32 @MPI_Wait(i8* %req, i8* null)
      call i32 @MPI_Request_free(i8* %req)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  const auto &summaries = analysis.getProcessModel().getRequestStateSummaries();
  ASSERT_EQ(summaries.size(), 1u);
  const auto &summary = summaries.begin()->second;
  EXPECT_TRUE(summary.is_persistent);
  EXPECT_EQ(summary.state, MPIRequestState::Freed);
  ASSERT_EQ(summary.history.size(), 4u);
  EXPECT_EQ(summary.history[0].action, MPIRequestActionKind::CreatePersistent);
  EXPECT_EQ(summary.history[1].action,
            MPIRequestActionKind::ActivatePersistent);
  EXPECT_EQ(summary.history[2].action, MPIRequestActionKind::CompleteMust);
  EXPECT_EQ(summary.history[3].action, MPIRequestActionKind::Free);
}

TEST_F(MPIAnalysisTest, StartallActivatesPersistentRequestArrays) {
  const char *source = R"(
    declare i32 @MPI_Send_init(i8*, i32, i32, i32, i32, i8*, i8*)
    declare i32 @MPI_Recv_init(i8*, i32, i32, i32, i32, i8*, i8*)
    declare i32 @MPI_Startall(i32, i8**)

    define i32 @main(i8* %comm) {
    entry:
      %req1 = alloca i8, align 1
      %req2 = alloca i8, align 1
      %reqs = alloca [2 x i8*], align 8
      %slot0 = getelementptr inbounds [2 x i8*], [2 x i8*]* %reqs, i64 0, i64 0
      %slot1 = getelementptr inbounds [2 x i8*], [2 x i8*]* %reqs, i64 0, i64 1
      store i8* %req1, i8** %slot0, align 8
      store i8* %req2, i8** %slot1, align 8
      call i32 @MPI_Send_init(i8* null, i32 1, i32 0, i32 1, i32 7, i8* %comm, i8* %req1)
      call i32 @MPI_Recv_init(i8* null, i32 1, i32 0, i32 2, i32 8, i8* %comm, i8* %req2)
      call i32 @MPI_Startall(i32 2, i8** %slot0)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  EXPECT_EQ(analysis.getResults().orphaned_requests.size(), 2u);
  const auto &request_sets = analysis.getRequestSetFacts();
  auto activation = std::find_if(
      request_sets.begin(), request_sets.end(),
      [](const MPIRequestSetFact &fact) {
        return fact.provenance == "mpi_request_set_activate";
      });
  ASSERT_NE(activation, request_sets.end());
  EXPECT_EQ(activation->arity, MPIRequestArity::Array);
  EXPECT_EQ(activation->completion_scope,
            MPIRequestCompletionScopeKind::AllOfSet);
}

TEST_F(MPIAnalysisTest,
       UnresolvedRequestArrayDoesNotCreateSyntheticSingletonRequest) {
  const char *source = R"(
    declare i32 @MPI_Isend(i8*, i32, i32, i32, i32, i8*, i8*)
    declare i32 @MPI_Waitall(i32, i8**, i8*)

    define i32 @main(i8* %comm, i1 %cond) {
    entry:
      %req1 = alloca i8, align 1
      %req2 = alloca i8, align 1
      %reqs = alloca [1 x i8*], align 8
      %slot0 = getelementptr inbounds [1 x i8*], [1 x i8*]* %reqs, i64 0, i64 0
      call i32 @MPI_Isend(i8* null, i32 1, i32 0, i32 1, i32 7, i8* %comm, i8* %req1)
      call i32 @MPI_Isend(i8* null, i32 1, i32 0, i32 2, i32 8, i8* %comm, i8* %req2)
      br i1 %cond, label %lhs, label %rhs

    lhs:
      store i8* %req1, i8** %slot0, align 8
      br label %join

    rhs:
      store i8* %req2, i8** %slot0, align 8
      br label %join

    join:
      call i32 @MPI_Waitall(i32 1, i8** %slot0, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  EXPECT_EQ(analysis.getRequestSetFacts().size(), 2u);
  bool saw_storage_gap = false;
  for (const MPIModelGap &gap : analysis.getResults().model_gaps) {
    if (gap.code == "mpi_request_storage_escaped") {
      saw_storage_gap = true;
      break;
    }
  }
  EXPECT_TRUE(saw_storage_gap);
}

TEST_F(MPIAnalysisTest,
       PointToPointDoesNotMatchAcrossDisjointSplitCommunicators) {
  const char *source = R"(
    declare i32 @MPI_Comm_split(i8*, i32, i32, i8**)
    declare i32 @MPI_Send(i8*, i32, i32, i32, i32, i8*)
    declare i32 @MPI_Recv(i8*, i32, i32, i32, i32, i8*, i8*)

    define i32 @rank0(i8* %comm) {
    entry:
      %split = alloca i8*, align 8
      call i32 @MPI_Comm_split(i8* %comm, i32 0, i32 0, i8** %split)
      %sub = load i8*, i8** %split, align 8
      call i32 @MPI_Send(i8* null, i32 1, i32 0, i32 1, i32 7, i8* %sub)
      ret i32 0
    }

    define i32 @rank1(i8* %comm) {
    entry:
      %split = alloca i8*, align 8
      %status = alloca i8, align 1
      call i32 @MPI_Comm_split(i8* %comm, i32 1, i32 0, i8** %split)
      %sub = load i8*, i8** %split, align 8
      call i32 @MPI_Recv(i8* null, i32 1, i32 0, i32 0, i32 7, i8* %sub, i8* %status)
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

  auto sends =
      analysis.getProcessModel().getOperationsByKind(MPIOpKind::SEND_BLOCKING);
  auto recvs =
      analysis.getProcessModel().getOperationsByKind(MPIOpKind::RECV_BLOCKING);
  ASSERT_EQ(sends.size(), 1u);
  ASSERT_EQ(recvs.size(), 1u);
  EXPECT_NE(sends.front().communicator_subgroup_id, 0u);
  EXPECT_NE(recvs.front().communicator_subgroup_id, 0u);
  EXPECT_NE(sends.front().communicator_subgroup_id,
            recvs.front().communicator_subgroup_id);
  EXPECT_EQ(analysis.getProcessModel().classifyCommunicationMatch(
                sends.front(), recvs.front()),
            MPICommunicationMatch::NoMatch);
}

TEST_F(MPIAnalysisTest, CancelTerminatesOutstandingRequest) {
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

  EXPECT_TRUE(analysis.getResults().orphaned_requests.empty());
}

TEST_F(MPIAnalysisTest, PrintResultsIncludesDetailedCounters) {
  const char *source = R"(
    declare i32 @MPI_Isend(i8*, i32, i32, i32, i32, i8*, i8*)
    declare i32 @MPI_Testany(i32, i8**, i32*, i32*, i8*)
    declare i32 @MPI_Ibarrier(i8*, i8*)
    declare i32 @MPI_Request_free(i8*)
    declare i32 @MPI_Win_create(i8*, i64, i32, i8*, i8*)
    declare i32 @MPI_Win_lock(i32, i32, i32, i8*)
    declare i32 @MPI_Put(i8*, i32, i32, i32, i64, i32, i32, i8*)
    declare i32 @MPI_Win_unlock(i32, i8*)
    @win = global i8 0, align 1

    define i32 @main(i8* %comm) {
    entry:
      %req1 = alloca i8, align 1
      %req2 = alloca i8, align 1
      %reqs = alloca [2 x i8*], align 8
      %slot0 = getelementptr inbounds [2 x i8*], [2 x i8*]* %reqs, i64 0, i64 0
      %slot1 = getelementptr inbounds [2 x i8*], [2 x i8*]* %reqs, i64 0, i64 1
      %index = alloca i32, align 4
      %flag = alloca i32, align 4
      store i8* %req1, i8** %slot0, align 8
      store i8* %req2, i8** %slot1, align 8
      store i32 1, i32* %flag, align 4
      call i32 @MPI_Isend(i8* null, i32 1, i32 0, i32 1, i32 7, i8* %comm, i8* %req1)
      call i32 @MPI_Isend(i8* null, i32 1, i32 0, i32 2, i32 8, i8* %comm, i8* %req2)
      call i32 @MPI_Ibarrier(i8* %comm, i8* %req2)
      call i32 @MPI_Testany(i32 2, i8** %slot0, i32* %index, i32* %flag, i8* null)
      call i32 @MPI_Request_free(i8* %req2)
      call i32 @MPI_Win_create(i8* null, i64 8, i32 4, i8* %comm, i8* @win)
      call i32 @MPI_Win_lock(i32 0, i32 1, i32 0, i8* @win)
      call i32 @MPI_Put(i8* null, i32 1, i32 0, i32 1, i64 0, i32 1, i32 0, i8* @win)
      call i32 @MPI_Win_unlock(i32 1, i8* @win)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  std::string output;
  raw_string_ostream os(output);
  analysis.printResults(os);
  os.flush();

  EXPECT_NE(output.find("MPI init/finalize ops: 0/0"), std::string::npos);
  EXPECT_NE(output.find("Blocking point-to-point ops: 0"), std::string::npos);
  EXPECT_NE(output.find("Non-blocking MPI operations: 3"), std::string::npos);
  EXPECT_NE(output.find("Non-blocking point-to-point ops: 2"),
            std::string::npos);
  EXPECT_NE(output.find("Wait/Test ops: 0/1"), std::string::npos);
  EXPECT_NE(output.find("RMA window lifecycle ops: 1"), std::string::npos);
  EXPECT_NE(output.find("Collective partial-reachability observations: 0"),
            std::string::npos);
  EXPECT_NE(output.find("Requests with may-complete status: 1"),
            std::string::npos);
  EXPECT_NE(output.find("Requests with terminal status: 1"), std::string::npos);
  EXPECT_NE(output.find("Requests with freed status: 1"), std::string::npos);
  EXPECT_NE(
      output.find(
          "Normalization confidence (exact/pmpi/openmpi-forwarder/unknown):"),
      std::string::npos);
  EXPECT_NE(output.find("Deferred MPI semantic lowering total: 1"),
            std::string::npos);
}

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

TEST_F(MPIAnalysisTest, OpenMPIInternalCollectiveNamesAreRecognized) {
  const char *source = R"(
    declare i32 @ompi_mpi_bcast(i8*, i32, i32, i32, i8*)

    define i32 @main(i8* %comm) {
    entry:
      call i32 @ompi_mpi_bcast(i8* null, i32 1, i32 0, i32 0, i8* %comm)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  EXPECT_EQ(analysis.getOperationCount(MPIOpKind::COLLECTIVE_BLOCKING), 1u);
}

TEST_F(MPIAnalysisTest, SemanticDescriptorCoverageForCoreMPITypes) {
  const std::vector<ThreadAPI::TD_TYPE> required_types = {
      ThreadAPI::TD_MPI_INIT,
      ThreadAPI::TD_MPI_FINALIZE,
      ThreadAPI::TD_MPI_SEND,
      ThreadAPI::TD_MPI_RECV,
      ThreadAPI::TD_MPI_SENDRECV,
      ThreadAPI::TD_MPI_ISEND,
      ThreadAPI::TD_MPI_IRECV,
      ThreadAPI::TD_MPI_WAIT,
      ThreadAPI::TD_MPI_TEST,
      ThreadAPI::TD_MPI_BARRIER,
      ThreadAPI::TD_MPI_BCAST,
      ThreadAPI::TD_MPI_REDUCE,
      ThreadAPI::TD_MPI_WIN_CREATE,
      ThreadAPI::TD_MPI_PUT,
      ThreadAPI::TD_MPI_WIN_LOCK,
      ThreadAPI::TD_MPI_COMM_DUP,
      ThreadAPI::TD_MPI_COMM_CREATE,
      ThreadAPI::TD_MPI_REQUEST_FREE,
      ThreadAPI::TD_MPI_TYPE_CONTIGUOUS,
      ThreadAPI::TD_MPI_TYPE_COMMIT,
      ThreadAPI::TD_MPI_SESSION_INIT,
      ThreadAPI::TD_MPI_SESSION_FINALIZE,
  };

  for (ThreadAPI::TD_TYPE type : required_types) {
    const MPISemanticDescriptor *descriptor = lookupMPISemantic(type);
    ASSERT_NE(descriptor, nullptr);
    if (type == ThreadAPI::TD_MPI_SEND || type == ThreadAPI::TD_MPI_RECV ||
        type == ThreadAPI::TD_MPI_ISEND || type == ThreadAPI::TD_MPI_IRECV) {
      EXPECT_NE(descriptor->communicator_arg, -1);
      EXPECT_NE(descriptor->count_arg, -1);
      EXPECT_NE(descriptor->datatype_arg, -1);
      EXPECT_NE(descriptor->peer_rank_arg, -1);
      EXPECT_NE(descriptor->tag_arg, -1);
    }
    if (type == ThreadAPI::TD_MPI_WAIT || type == ThreadAPI::TD_MPI_WAITALL ||
        type == ThreadAPI::TD_MPI_TEST || type == ThreadAPI::TD_MPI_TESTALL) {
      EXPECT_NE(descriptor->request_arg, -1);
    }
  }
}

TEST_F(MPIAnalysisTest,
       SemanticDescriptorCoverageForNewStatusAndTopologyTypes) {
  const MPISemanticDescriptor *get_count =
      lookupMPISemantic(ThreadAPI::TD_MPI_GET_COUNT);
  ASSERT_NE(get_count, nullptr);
  EXPECT_EQ(get_count->kind, MPIOpKind::REQUEST_MANAGEMENT);
  EXPECT_EQ(get_count->family, MPISemanticFamily::Request);

  const MPISemanticDescriptor *status_set =
      lookupMPISemantic(ThreadAPI::TD_MPI_STATUS_SET_ELEMENTS);
  ASSERT_NE(status_set, nullptr);
  EXPECT_EQ(status_set->kind, MPIOpKind::REQUEST_MANAGEMENT);
  EXPECT_EQ(status_set->family, MPISemanticFamily::Request);

  const MPISemanticDescriptor *cart_create =
      lookupMPISemantic(ThreadAPI::TD_MPI_CART_CREATE);
  ASSERT_NE(cart_create, nullptr);
  EXPECT_EQ(cart_create->kind, MPIOpKind::COMM_MANAGEMENT);
  EXPECT_EQ(cart_create->family, MPISemanticFamily::Communicator);

  const MPISemanticDescriptor *dist_graph_neighbors =
      lookupMPISemantic(ThreadAPI::TD_MPI_DIST_GRAPH_NEIGHBORS);
  ASSERT_NE(dist_graph_neighbors, nullptr);
  EXPECT_EQ(dist_graph_neighbors->kind, MPIOpKind::COMM_MANAGEMENT);
  EXPECT_EQ(dist_graph_neighbors->family, MPISemanticFamily::Communicator);

  const MPISemanticDescriptor *graph_map =
      lookupMPISemantic(ThreadAPI::TD_MPI_GRAPH_MAP);
  ASSERT_NE(graph_map, nullptr);
  EXPECT_EQ(graph_map->kind, MPIOpKind::COMM_MANAGEMENT);
  EXPECT_EQ(graph_map->family, MPISemanticFamily::Communicator);
}

TEST_F(MPIAnalysisTest, MPISpecDescriptorsAreExplicitlyCovered) {
  std::ifstream spec(
      "/Users/rainoftime/Work/analysis/lotus/config/mpi_api.spec");
  ASSERT_TRUE(spec.is_open());

  auto isExplicitlyIgnored = [](const std::string &type_name) {
    return type_name.find("ERRHANDLER") != std::string::npos ||
           type_name.find("INFO_") != std::string::npos ||
           type_name.find("STATUS_") != std::string::npos ||
           type_name.find("ERROR_") != std::string::npos ||
           type_name.find("GRAPH_") != std::string::npos ||
           type_name.find("CART_") != std::string::npos ||
           type_name.find("DIST_GRAPH_") != std::string::npos ||
           type_name.find("GET_COUNT") != std::string::npos ||
           type_name.find("GET_ELEMENTS") != std::string::npos;
  };

  std::string line;
  size_t checked = 0;
  while (std::getline(spec, line)) {
    if (line.empty() || line[0] == '#') {
      continue;
    }

    std::istringstream iss(line);
    std::string symbol;
    std::string type_name;
    if (!(iss >> symbol >> type_name)) {
      continue;
    }
    if (type_name.find("TD_MPI_") != 0) {
      continue;
    }

    LLVMContext local_context;
    Module local_module("mpi_spec_probe", local_context);
    auto *fn_ty = FunctionType::get(Type::getInt32Ty(local_context), false);
    Function *fn = Function::Create(fn_ty, Function::ExternalLinkage, symbol,
                                    local_module);
    ThreadAPI::TD_TYPE type = ThreadAPI::getThreadAPI()->getType(fn);
    if (type == ThreadAPI::TD_DUMMY) {
      continue;
    }
    if (isExplicitlyIgnored(type_name)) {
      continue;
    }

    EXPECT_NE(lookupMPISemantic(type), nullptr)
        << "missing descriptor for " << symbol << " " << type_name;
    ++checked;
  }

  EXPECT_GT(checked, 50u);
}

TEST_F(MPIAnalysisTest, NeighborCollectivesUseDistinctProtocolClass) {
  const char *source = R"(
    declare i32 @MPI_Alltoall(i8*, i32, i32, i8*, i32, i32, i8*)
    declare i32 @MPI_Neighbor_alltoall(i8*, i32, i32, i8*, i32, i32, i8*)

    define void @rank0(i8* %comm) {
    entry:
      call i32 @MPI_Alltoall(i8* null, i32 1, i32 2, i8* null, i32 1, i32 2, i8* %comm)
      ret void
    }

    define void @rank1(i8* %comm) {
    entry:
      call i32 @MPI_Neighbor_alltoall(i8* null, i32 4, i32 2, i8* null, i32 4, i32 2, i8* %comm)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  EXPECT_EQ(analysis.getResults().mismatched_collectives.size(), 0u);
}

TEST_F(MPIAnalysisTest, IntercommunicatorCreateGetsDedicatedOperationKind) {
  const char *source = R"(
    declare i32 @MPI_Intercomm_create(i8*, i32, i8*, i32, i32, i8**)

    define i32 @main(i8* %local, i8* %peer) {
    entry:
      %out = alloca i8*, align 8
      call i32 @MPI_Intercomm_create(i8* %local, i32 0, i8* %peer, i32 0, i32 7, i8** %out)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  EXPECT_EQ(analysis.getOperationCount(MPIOpKind::INTERCOMM_CREATION), 1u);
}

TEST_F(MPIAnalysisTest, DerivedDatatypeExtentPropagatesToPointToPointOps) {
  const char *source = R"(
    declare i32 @MPI_Type_contiguous(i32, i32, i8*)
    declare i32 @MPI_Send(i8*, i32, i8*, i32, i32, i8*)

    define i32 @main(i8* %comm) {
    entry:
      %derived_type = alloca i8, align 1
      call i32 @MPI_Type_contiguous(i32 4, i32 2, i8* %derived_type)
      call i32 @MPI_Send(i8* null, i32 3, i8* %derived_type, i32 1, i32 7, i8* %comm)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  auto sends =
      analysis.getProcessModel().getOperationsByKind(MPIOpKind::SEND_BLOCKING);
  ASSERT_EQ(sends.size(), 1u);
  EXPECT_EQ(sends.front().datatype_size, 16);
  EXPECT_EQ(sends.front().byte_length, 48);
}

TEST_F(MPIAnalysisTest, WrappedCommRankStillRefinesConditionalCollectives) {
  const char *source = R"(
    declare i32 @__wrap_MPI_Comm_rank(i8*, i32*)
    declare i32 @MPI_Bcast(i8*, i32, i32, i32, i8*)

    define i32 @main(i8* %comm) {
    entry:
      %rank = alloca i32, align 4
      call i32 @__wrap_MPI_Comm_rank(i8* %comm, i32* %rank)
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
}

TEST_F(MPIAnalysisTest, RMAOpBeforeLockEpochRemainsUnsynchronized) {
  const char *source = R"(
    declare i32 @MPI_Win_create(i8*, i64, i32, i8*, i8*)
    declare i32 @MPI_Win_lock_all(i32, i8*)
    declare i32 @MPI_Win_unlock_all(i8*)
    declare i32 @MPI_Put(i8*, i32, i32, i32, i64, i32, i32, i8*)

    define i32 @main(i8* %comm) {
    entry:
      %win = alloca i8, align 1
      call i32 @MPI_Win_create(i8* null, i64 16, i32 4, i8* %comm, i8* %win)
      call i32 @MPI_Put(i8* null, i32 1, i32 0, i32 0, i64 0, i32 1, i32 0, i8* %win)
      call i32 @MPI_Win_lock_all(i32 0, i8* %win)
      call i32 @MPI_Win_unlock_all(i8* %win)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  EXPECT_EQ(analysis.getResults().unsynchronized_rma.size(), 1u);
}

TEST_F(MPIAnalysisTest, RMAOpInsideLockEpochIsSynchronized) {
  const char *source = R"(
    declare i32 @MPI_Win_create(i8*, i64, i32, i8*, i8*)
    declare i32 @MPI_Win_lock_all(i32, i8*)
    declare i32 @MPI_Win_flush_all(i8*)
    declare i32 @MPI_Win_unlock_all(i8*)
    declare i32 @MPI_Put(i8*, i32, i32, i32, i64, i32, i32, i8*)

    define i32 @main(i8* %comm) {
    entry:
      %win = alloca i8, align 1
      call i32 @MPI_Win_create(i8* null, i64 16, i32 4, i8* %comm, i8* %win)
      call i32 @MPI_Win_lock_all(i32 0, i8* %win)
      call i32 @MPI_Put(i8* null, i32 1, i32 0, i32 0, i64 0, i32 1, i32 0, i8* %win)
      call i32 @MPI_Win_flush_all(i8* %win)
      call i32 @MPI_Win_unlock_all(i8* %win)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  EXPECT_TRUE(analysis.getResults().unsynchronized_rma.empty());
  EXPECT_FALSE(analysis.getResults().rma_synchronization_facts.empty());
}

TEST_F(MPIAnalysisTest, RMAOpsOnOtherTargetsDoNotReusePointLockEpoch) {
  const char *source = R"(
    declare i32 @MPI_Win_create(i8*, i64, i32, i8*, i8*)
    declare i32 @MPI_Win_lock(i32, i32, i32, i8*)
    declare i32 @MPI_Win_flush(i32, i8*)
    declare i32 @MPI_Win_unlock(i32, i8*)
    declare i32 @MPI_Put(i8*, i32, i32, i32, i64, i32, i32, i8*)

    define i32 @main(i8* %comm) {
    entry:
      %win = alloca i8, align 1
      call i32 @MPI_Win_create(i8* null, i64 16, i32 4, i8* %comm, i8* %win)
      call i32 @MPI_Win_lock(i32 0, i32 0, i32 0, i8* %win)
      call i32 @MPI_Put(i8* null, i32 1, i32 0, i32 0, i64 0, i32 1, i32 0, i8* %win)
      call i32 @MPI_Put(i8* null, i32 1, i32 0, i32 1, i64 0, i32 1, i32 0, i8* %win)
      call i32 @MPI_Win_flush(i32 0, i8* %win)
      call i32 @MPI_Win_unlock(i32 0, i8* %win)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  ASSERT_EQ(analysis.getResults().unsynchronized_rma.size(), 1u);
  const Instruction *unsync_inst = analysis.getResults().unsynchronized_rma.front().inst;
  ASSERT_NE(unsync_inst, nullptr);
  const auto *cb = dyn_cast<CallBase>(unsync_inst);
  ASSERT_NE(cb, nullptr);
  ASSERT_EQ(cb->arg_size(), 8u);
  const auto *rank = dyn_cast<ConstantInt>(cb->getArgOperand(3));
  ASSERT_NE(rank, nullptr);
  EXPECT_EQ(rank->getSExtValue(), 1);
}

TEST_F(MPIAnalysisTest, PSCWAccessEpochSynchronizesContainedOps) {
  const char *source = R"(
    declare i32 @MPI_Win_create(i8*, i64, i32, i8*, i8*)
    declare i32 @MPI_Win_start(i8*, i32, i8*)
    declare i32 @MPI_Win_complete(i8*)
    declare i32 @MPI_Put(i8*, i32, i32, i32, i64, i32, i32, i8*)

    define i32 @main(i8* %comm) {
    entry:
      %group = alloca i8, align 1
      %win = alloca i8, align 1
      call i32 @MPI_Win_create(i8* null, i64 16, i32 4, i8* %comm, i8* %win)
      call i32 @MPI_Win_start(i8* %group, i32 0, i8* %win)
      call i32 @MPI_Put(i8* null, i32 1, i32 0, i32 0, i64 0, i32 1, i32 0, i8* %win)
      call i32 @MPI_Win_complete(i8* %win)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  EXPECT_FALSE(analysis.getResults().unsynchronized_rma.empty());
}

TEST_F(MPIAnalysisTest, LatestFlagStoreBeforeTestCompletesRequest) {
  const char *source = R"(
    declare i32 @MPI_Isend(i8*, i32, i32, i32, i32, i8*, i8*)
    declare i32 @MPI_Test(i8*, i32*, i8*)

    define i32 @main(i8* %comm) {
    entry:
      %req = alloca i8, align 1
      %flag = alloca i32, align 4
      store i32 0, i32* %flag, align 4
      call i32 @MPI_Isend(i8* null, i32 1, i32 0, i32 1, i32 7, i8* %comm, i8* %req)
      store i32 1, i32* %flag, align 4
      call i32 @MPI_Test(i8* %req, i32* %flag, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  EXPECT_TRUE(analysis.getResults().orphaned_requests.empty());
}

TEST_F(MPIAnalysisTest, DisjointRMADisplacementsDoNotRace) {
  const char *source = R"(
    declare i32 @MPI_Win_create(i8*, i64, i32, i8*, i8*)
    declare i32 @MPI_Win_lock_all(i32, i8*)
    declare i32 @MPI_Win_unlock_all(i8*)
    declare i32 @MPI_Put(i8*, i32, i32, i32, i64, i32, i32, i8*)

    define i32 @main(i8* %comm) {
    entry:
      %win = alloca i8, align 1
      call i32 @MPI_Win_create(i8* null, i64 16, i32 4, i8* %comm, i8* %win)
      call i32 @MPI_Win_lock_all(i32 0, i8* %win)
      call i32 @MPI_Put(i8* null, i32 1, i32 0, i32 0, i64 0, i32 1, i32 0, i8* %win)
      call i32 @MPI_Put(i8* null, i32 1, i32 0, i32 0, i64 8, i32 1, i32 0, i8* %win)
      call i32 @MPI_Win_unlock_all(i8* %win)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  EXPECT_TRUE(analysis.getResults().rma_races.empty());
}

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

TEST_F(MPIAnalysisTest, PSCWExposureEpochSynchronizesContainedOps) {
  const char *source = R"(
    declare i32 @MPI_Win_create(i8*, i64, i32, i8*, i8*)
    declare i32 @MPI_Win_post(i8*, i32, i8*)
    declare i32 @MPI_Win_wait(i8*)
    declare i32 @MPI_Put(i8*, i32, i32, i32, i64, i32, i32, i8*)

    define i32 @main(i8* %comm) {
    entry:
      %group = alloca i8, align 1
      %win = alloca i8, align 1
      call i32 @MPI_Win_create(i8* null, i64 16, i32 4, i8* %comm, i8* %win)
      call i32 @MPI_Win_post(i8* %group, i32 0, i8* %win)
      call i32 @MPI_Put(i8* null, i32 1, i32 0, i32 0, i64 0, i32 1, i32 0, i8* %win)
      call i32 @MPI_Win_wait(i8* %win)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  EXPECT_FALSE(analysis.getResults().unsynchronized_rma.empty());
}

TEST_F(MPIAnalysisTest, LonePSCWEpochProducesUnresolvedGroupFact) {
  const char *source = R"(
    declare i32 @MPI_Win_create(i8*, i64, i32, i8*, i8*)
    declare i32 @MPI_Win_start(i8*, i32, i8*)
    declare i32 @MPI_Win_complete(i8*)
    declare i32 @MPI_Put(i8*, i32, i32, i32, i64, i32, i32, i8*)

    define i32 @main(i8* %comm) {
    entry:
      %group = alloca i8, align 1
      %win = alloca i8, align 1
      call i32 @MPI_Win_create(i8* null, i64 16, i32 4, i8* %comm, i8* %win)
      call i32 @MPI_Win_start(i8* %group, i32 0, i8* %win)
      call i32 @MPI_Put(i8* null, i32 1, i32 0, i32 0, i64 0, i32 1, i32 0, i8* %win)
      call i32 @MPI_Win_complete(i8* %win)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  bool saw_unresolved_group = false;
  for (const auto &fact : analysis.getResults().rma_synchronization_facts) {
    if (fact.code == "mpi_rma_pscw_group_unresolved") {
      saw_unresolved_group = true;
    }
  }
  EXPECT_TRUE(saw_unresolved_group);
  EXPECT_FALSE(analysis.getResults().unsynchronized_rma.empty());
}

TEST_F(MPIAnalysisTest, BlockingAndNonBlockingCollectivesDoNotCompareAsCompatible) {
  const char *source = R"(
    declare i32 @MPI_Bcast(i8*, i32, i32, i32, i8*)
    declare i32 @MPI_Ibcast(i8*, i32, i32, i32, i8*, i8*)

    define i32 @rank0(i8* %comm) {
    entry:
      %req = alloca i8, align 1
      call i32 @MPI_Bcast(i8* null, i32 1, i32 0, i32 0, i8* %comm)
      ret i32 0
    }

    define i32 @rank1(i8* %comm) {
    entry:
      %req = alloca i8, align 1
      call i32 @MPI_Ibcast(i8* null, i32 1, i32 0, i32 0, i8* %comm, i8* %req)
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

TEST_F(MPIAnalysisTest, DistinctCollectiveVariantsRemainIncompatible) {
  const char *source = R"(
    declare i32 @MPI_Alltoall(i8*, i32, i32, i8*, i32, i32, i8*)
    declare i32 @MPI_Alltoallv(i8*, i32*, i32*, i32, i8*, i32*, i32*, i32, i8*)

    define i32 @rank0(i8* %comm) {
    entry:
      call i32 @MPI_Alltoall(i8* null, i32 1, i32 0, i8* null, i32 1, i32 0, i8* %comm)
      ret i32 0
    }

    define i32 @rank1(i8* %comm) {
    entry:
      %counts = alloca i32, align 4
      %displs = alloca i32, align 4
      call i32 @MPI_Alltoallv(i8* null, i32* %counts, i32* %displs, i32 0,
                              i8* null, i32* %counts, i32* %displs, i32 0, i8* %comm)
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

TEST_F(MPIAnalysisTest, DistinctPSCWGroupsStayUnresolved) {
  const char *source = R"(
    declare i32 @MPI_Win_create(i8*, i64, i32, i8*, i8*)
    declare i32 @MPI_Win_start(i8*, i32, i8*)
    declare i32 @MPI_Win_complete(i8*)
    declare i32 @MPI_Win_post(i8*, i32, i8*)
    declare i32 @MPI_Win_wait(i8*)
    declare i32 @MPI_Put(i8*, i32, i32, i32, i64, i32, i32, i8*)

    define i32 @access_rank(i8* %comm) {
    entry:
      %group_a = alloca i8, align 1
      %win = alloca i8, align 1
      call i32 @MPI_Win_create(i8* null, i64 16, i32 4, i8* %comm, i8* %win)
      call i32 @MPI_Win_start(i8* %group_a, i32 0, i8* %win)
      call i32 @MPI_Put(i8* null, i32 1, i32 0, i32 0, i64 0, i32 1, i32 0, i8* %win)
      call i32 @MPI_Win_complete(i8* %win)
      ret i32 0
    }

    define i32 @exposure_rank(i8* %comm) {
    entry:
      %group_b = alloca i8, align 1
      %win = alloca i8, align 1
      call i32 @MPI_Win_create(i8* null, i64 16, i32 4, i8* %comm, i8* %win)
      call i32 @MPI_Win_post(i8* %group_b, i32 0, i8* %win)
      call i32 @MPI_Win_wait(i8* %win)
      ret i32 0
    }

    define i32 @main(i8* %comm) {
    entry:
      %a = call i32 @access_rank(i8* %comm)
      %b = call i32 @exposure_rank(i8* %comm)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  bool saw_unresolved_group = false;
  for (const auto &fact : analysis.getResults().rma_synchronization_facts) {
    if (fact.code == "mpi_rma_pscw_group_unresolved") {
      saw_unresolved_group = true;
      EXPECT_NE(fact.relation.proof, concurrency::ProofStrength::Must);
    }
  }
  EXPECT_TRUE(saw_unresolved_group);
}

TEST_F(MPIAnalysisTest,
       InvalidRMAEpochTransitionLeavesOperationUnsynchronized) {
  const char *source = R"(
    declare i32 @MPI_Win_create(i8*, i64, i32, i8*, i8*)
    declare i32 @MPI_Win_lock(i32, i32, i32, i8*)
    declare i32 @MPI_Win_fence(i32, i8*)
    declare i32 @MPI_Win_unlock(i32, i8*)
    declare i32 @MPI_Put(i8*, i32, i32, i32, i64, i32, i32, i8*)

    define i32 @main(i8* %comm) {
    entry:
      %win = alloca i8, align 1
      call i32 @MPI_Win_create(i8* null, i64 16, i32 4, i8* %comm, i8* %win)
      call i32 @MPI_Win_lock(i32 0, i32 0, i32 0, i8* %win)
      call i32 @MPI_Put(i8* null, i32 1, i32 0, i32 0, i64 0, i32 1, i32 0, i8* %win)
      call i32 @MPI_Win_fence(i32 0, i8* %win)
      call i32 @MPI_Win_unlock(i32 0, i8* %win)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  ASSERT_EQ(analysis.getResults().unsynchronized_rma.size(), 1u);
  EXPECT_EQ(analysis.getResults().invalid_rma_transitions.size(), 2u);
  const auto &relations =
      analysis.getRMAAnalysis().getSynchronizationRelations();
  ASSERT_EQ(relations.size(), 1u);
  EXPECT_EQ(relations.front().relation.kind,
            concurrency::RelationKind::UnknownDueToModelGap);
}

TEST_F(MPIAnalysisTest, UseAfterFreeWindowIsReported) {
  const char *source = R"(
    declare i32 @MPI_Win_create(i8*, i64, i32, i8*, i8*)
    declare i32 @MPI_Win_free(i8*)
    declare i32 @MPI_Put(i8*, i32, i32, i32, i64, i32, i32, i8*)

    define i32 @main(i8* %comm) {
    entry:
      %win = alloca i8, align 1
      call i32 @MPI_Win_create(i8* null, i64 16, i32 4, i8* %comm, i8* %win)
      call i32 @MPI_Win_free(i8* %win)
      call i32 @MPI_Put(i8* null, i32 1, i32 0, i32 0, i64 0, i32 1, i32 0, i8* %win)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  EXPECT_EQ(analysis.getResults().use_after_free_windows.size(), 1u);
}

TEST_F(MPIAnalysisTest, OperationBeforeWindowFreeIsNotUseAfterFree) {
  const char *source = R"(
    declare i32 @MPI_Win_create(i8*, i64, i32, i8*, i8*)
    declare i32 @MPI_Win_free(i8*)
    declare i32 @MPI_Put(i8*, i32, i32, i32, i64, i32, i32, i8*)

    define i32 @main(i8* %comm) {
    entry:
      %win = alloca i8, align 1
      call i32 @MPI_Win_create(i8* null, i64 16, i32 4, i8* %comm, i8* %win)
      call i32 @MPI_Put(i8* null, i32 1, i32 0, i32 0, i64 0, i32 1, i32 0, i8* %win)
      call i32 @MPI_Win_free(i8* %win)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  EXPECT_TRUE(analysis.getResults().use_after_free_windows.empty());
}

TEST_F(MPIAnalysisTest, BranchSeparatedWindowFreeDoesNotReportUseAfterFree) {
  const char *source = R"(
    declare i32 @MPI_Win_create(i8*, i64, i32, i8*, i8*)
    declare i32 @MPI_Win_free(i8*)
    declare i32 @MPI_Put(i8*, i32, i32, i32, i64, i32, i32, i8*)

    define i32 @main(i8* %comm, i1 %cond) {
    entry:
      %win = alloca i8, align 1
      call i32 @MPI_Win_create(i8* null, i64 16, i32 4, i8* %comm, i8* %win)
      br i1 %cond, label %free_path, label %use_path

    free_path:
      call i32 @MPI_Win_free(i8* %win)
      br label %join

    use_path:
      call i32 @MPI_Put(i8* null, i32 1, i32 0, i32 0, i64 0, i32 1, i32 0, i8* %win)
      br label %join

    join:
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  EXPECT_TRUE(analysis.getResults().use_after_free_windows.empty());
}

TEST_F(MPIAnalysisTest,
       CrossFunctionWindowFreeWithoutOrderingProducesModelGap) {
  const char *source = R"(
    @win = global i8 0, align 1
    declare i32 @MPI_Win_create(i8*, i64, i32, i8*, i8*)
    declare i32 @MPI_Win_free(i8*)
    declare i32 @MPI_Put(i8*, i32, i32, i32, i64, i32, i32, i8*)

    define void @free_win() {
    entry:
      call i32 @MPI_Win_free(i8* @win)
      ret void
    }

    define void @use_win() {
    entry:
      call i32 @MPI_Put(i8* null, i32 1, i32 0, i32 0, i64 0, i32 1, i32 0,
                        i8* @win)
      ret void
    }

    define i32 @main(i8* %comm, i1 %cond) {
    entry:
      call i32 @MPI_Win_create(i8* null, i64 16, i32 4, i8* %comm, i8* @win)
      br i1 %cond, label %free_path, label %use_path

    free_path:
      call void @free_win()
      br label %join

    use_path:
      call void @use_win()
      br label %join

    join:
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  EXPECT_TRUE(analysis.getResults().use_after_free_windows.empty());
  bool saw_gap = false;
  for (const auto &gap : analysis.getResults().model_gaps) {
    if (gap.code == "mpi_rma_use_after_free_order_unresolved") {
      saw_gap = true;
      break;
    }
  }
  EXPECT_TRUE(saw_gap);
}

TEST_F(MPIAnalysisTest, RMAEpochRelationFlowsIntoDiagnostics) {
  const char *source = R"(
    declare i32 @MPI_Win_create(i8*, i64, i32, i8*, i8*)
    declare i32 @MPI_Win_lock(i32, i32, i32, i8*)
    declare i32 @MPI_Put(i8*, i32, i32, i32, i64, i32, i32, i8*)
    declare i32 @MPI_Win_unlock(i32, i8*)

    define i32 @main(i8* %comm) {
    entry:
      %win = alloca i8, align 1
      call i32 @MPI_Win_create(i8* null, i64 16, i32 4, i8* %comm, i8* %win)
      call i32 @MPI_Win_lock(i32 0, i32 0, i32 0, i8* %win)
      call i32 @MPI_Put(i8* null, i32 1, i32 0, i32 0, i64 0, i32 1, i32 0, i8* %win)
      call i32 @MPI_Win_unlock(i32 0, i8* %win)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  bool saw_epoch_relation = false;
  for (const auto &diag : analysis.getResults().diagnostics) {
    if (diag.relation.kind ==
            concurrency::RelationKind::SameSynchronizationEpoch &&
        diag.relation.reason == "mpi_rma_lock_epoch") {
      saw_epoch_relation = true;
      break;
    }
  }
  EXPECT_TRUE(saw_epoch_relation);
}

TEST_F(MPIAnalysisTest, DoubleWindowFreeIsReported) {
  const char *source = R"(
    declare i32 @MPI_Win_create(i8*, i64, i32, i8*, i8*)
    declare i32 @MPI_Win_free(i8*)

    define i32 @main(i8* %comm) {
    entry:
      %win = alloca i8, align 1
      call i32 @MPI_Win_create(i8* null, i64 16, i32 4, i8* %comm, i8* %win)
      call i32 @MPI_Win_free(i8* %win)
      call i32 @MPI_Win_free(i8* %win)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  EXPECT_EQ(analysis.getResults().double_window_free.size(), 1u);
}

TEST_F(MPIAnalysisTest, CommunicatorSplitSharesStableSubgroupAcrossRanks) {
  const char *source = R"(
    declare i32 @MPI_Comm_split(i8*, i32, i32, i8**)
    declare i32 @MPI_Bcast(i8*, i32, i32, i32, i8*)

    define i32 @rank0(i8* %comm) {
    entry:
      %split = alloca i8*, align 8
      call i32 @MPI_Comm_split(i8* %comm, i32 7, i32 0, i8** %split)
      %split_loaded = load i8*, i8** %split, align 8
      call i32 @MPI_Bcast(i8* null, i32 1, i32 0, i32 0, i8* %split_loaded)
      ret i32 0
    }

    define i32 @rank1(i8* %comm) {
    entry:
      %split = alloca i8*, align 8
      call i32 @MPI_Comm_split(i8* %comm, i32 7, i32 1, i8** %split)
      %split_loaded = load i8*, i8** %split, align 8
      call i32 @MPI_Bcast(i8* null, i32 1, i32 0, i32 0, i8* %split_loaded)
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

  auto collectives = analysis.getProcessModel().getOperationsByKind(
      MPIOpKind::COLLECTIVE_BLOCKING);
  ASSERT_EQ(collectives.size(), 2u);
  EXPECT_NE(collectives[0].communicator_subgroup_id, 0u);
  EXPECT_NE(collectives[1].communicator_subgroup_id, 0u);
  EXPECT_TRUE(analysis.getResults().mismatched_collectives.empty());
}

TEST_F(MPIAnalysisTest, UnknownSplitColorProducesExplicitSubgroupModelGap) {
  const char *source = R"(
    declare i32 @MPI_Comm_split(i8*, i32, i32, i8**)
    declare i32 @MPI_Bcast(i8*, i32, i32, i32, i8*)

    define i32 @main(i8* %comm, i32 %color) {
    entry:
      %split = alloca i8*, align 8
      call i32 @MPI_Comm_split(i8* %comm, i32 %color, i32 0, i8** %split)
      %split_loaded = load i8*, i8** %split, align 8
      call i32 @MPI_Bcast(i8* null, i32 1, i32 0, i32 0, i8* %split_loaded)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  bool saw_subgroup_gap = false;
  for (const auto &gap : analysis.getResults().model_gaps) {
    if (gap.code == "mpi_subgroup_identity_unresolved") {
      saw_subgroup_gap = true;
      EXPECT_NE(gap.subgroup_id, 0u);
    }
  }
  EXPECT_TRUE(saw_subgroup_gap);

  auto collectives = analysis.getProcessModel().getOperationsByKind(
      MPIOpKind::COLLECTIVE_BLOCKING);
  ASSERT_EQ(collectives.size(), 1u);
  EXPECT_NE(collectives[0].communicator_subgroup_id, 0u);
}

TEST_F(MPIAnalysisTest, CommunicatorDupPreservesSplitSubgroupFacts) {
  const char *source = R"(
    declare i32 @MPI_Comm_split(i8*, i32, i32, i8**)
    declare i32 @MPI_Comm_dup(i8*, i8**)
    declare i32 @MPI_Bcast(i8*, i32, i32, i32, i8*)

    define i32 @main(i8* %world) {
    entry:
      %sub = alloca i8*, align 8
      %dup = alloca i8*, align 8
      call i32 @MPI_Comm_split(i8* %world, i32 0, i32 7, i8** %sub)
      %child = load i8*, i8** %sub, align 8
      call i32 @MPI_Comm_dup(i8* %child, i8** %dup)
      %dup_loaded = load i8*, i8** %dup, align 8
      call i32 @MPI_Bcast(i8* null, i32 1, i32 0, i32 0, i8* %child)
      call i32 @MPI_Bcast(i8* null, i32 1, i32 0, i32 0, i8* %dup_loaded)
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
  EXPECT_EQ(collectives[0].communicator_class_id, collectives[1].communicator_class_id);
  EXPECT_EQ(collectives[0].communicator_subgroup_id,
            collectives[1].communicator_subgroup_id);
}

TEST_F(MPIAnalysisTest, UnrelatedCommunicatorArgumentsDoNotCollapseByPosition) {
  const char *source = R"(
    declare i32 @MPI_Bcast(i8*, i32, i32, i32, i8*)

    define void @left(i8* %comm_left) {
    entry:
      call i32 @MPI_Bcast(i8* null, i32 1, i32 0, i32 0, i8* %comm_left)
      ret void
    }

    define void @right(i8* %comm_right) {
    entry:
      call i32 @MPI_Bcast(i8* null, i32 1, i32 0, i32 0, i8* %comm_right)
      ret void
    }

    define i32 @main(i8* %comm0, i8* %comm1) {
    entry:
      call void @left(i8* %comm0)
      call void @right(i8* %comm1)
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
  EXPECT_NE(collectives[0].communicator_class_id, 0u);
  EXPECT_NE(collectives[1].communicator_class_id, 0u);
  EXPECT_NE(collectives[0].communicator_class_id,
            collectives[1].communicator_class_id);
}

TEST_F(MPIAnalysisTest,
       AmbiguousHelperCommunicatorDoesNotCollapseAndEmitsModelGap) {
  const char *source = R"(
    declare i32 @MPI_Bcast(i8*, i32, i32, i32, i8*)

    define void @helper(i8* %comm) {
    entry:
      call i32 @MPI_Bcast(i8* null, i32 1, i32 0, i32 0, i8* %comm)
      ret void
    }

    define i32 @main(i8* %comm0, i8* %comm1) {
    entry:
      call void @helper(i8* %comm0)
      call void @helper(i8* %comm1)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  auto collectives = analysis.getProcessModel().getOperationsByKind(
      MPIOpKind::COLLECTIVE_BLOCKING);
  ASSERT_EQ(collectives.size(), 1u);
  EXPECT_EQ(collectives[0].communicator_class_id, 0u);

  size_t gap_count = 0;
  for (const auto &gap : analysis.getResults().model_gaps) {
    if (gap.code == "mpi_communicator_identity_ambiguous") {
      ++gap_count;
    }
  }
  EXPECT_GE(gap_count, 1u);
}

TEST_F(MPIAnalysisTest, HelperCommunicatorReusedFromSameRootStillUnifies) {
  const char *source = R"(
    declare i32 @MPI_Bcast(i8*, i32, i32, i32, i8*)

    define void @helper(i8* %comm) {
    entry:
      call i32 @MPI_Bcast(i8* null, i32 1, i32 0, i32 0, i8* %comm)
      ret void
    }

    define i32 @main(i8* %comm0) {
    entry:
      call void @helper(i8* %comm0)
      call void @helper(i8* %comm0)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  auto collectives = analysis.getProcessModel().getOperationsByKind(
      MPIOpKind::COLLECTIVE_BLOCKING);
  ASSERT_EQ(collectives.size(), 1u);
  EXPECT_NE(collectives[0].communicator_class_id, 0u);
  bool saw_ambiguous_gap = false;
  for (const auto &gap : analysis.getResults().model_gaps) {
    if (gap.code == "mpi_communicator_identity_ambiguous") {
      saw_ambiguous_gap = true;
      break;
    }
  }
  EXPECT_FALSE(saw_ambiguous_gap);
}

TEST_F(MPIAnalysisTest, LoadedCommunicatorStillParticipatesInCollectiveMismatchChecks) {
  const char *source = R"(
    declare i32 @MPI_Bcast(i8*, i32, i32, i32, i8*)
    declare i32 @MPI_Reduce(i8*, i8*, i32, i32, i32, i32, i8*)

    define void @rank0(i8** %comm_slot) {
    entry:
      %comm = load i8*, i8** %comm_slot
      call i32 @MPI_Bcast(i8* null, i32 1, i32 0, i32 0, i8* %comm)
      ret void
    }

    define void @rank1(i8** %comm_slot) {
    entry:
      %comm = load i8*, i8** %comm_slot
      call i32 @MPI_Reduce(i8* null, i8* null, i32 1, i32 0, i32 0, i32 0, i8* %comm)
      ret void
    }

    define i32 @main(i8* %comm) {
    entry:
      %slot = alloca i8*
      store i8* %comm, i8** %slot
      call void @rank0(i8** %slot)
      call void @rank1(i8** %slot)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  EXPECT_EQ(analysis.getResults().mismatched_collectives.size(), 1u);
}

TEST_F(MPIAnalysisTest, LoadedCommunicatorStillParticipatesInWrongRootChecks) {
  const char *source = R"(
    declare i32 @MPI_Bcast(i8*, i32, i32, i32, i8*)

    define void @rank0(i8** %comm_slot) {
    entry:
      %comm = load i8*, i8** %comm_slot
      call i32 @MPI_Bcast(i8* null, i32 1, i32 0, i32 0, i8* %comm)
      ret void
    }

    define void @rank1(i8** %comm_slot) {
    entry:
      %comm = load i8*, i8** %comm_slot
      call i32 @MPI_Bcast(i8* null, i32 1, i32 0, i32 1, i8* %comm)
      ret void
    }

    define i32 @main(i8* %comm) {
    entry:
      %slot = alloca i8*
      store i8* %comm, i8** %slot
      call void @rank0(i8** %slot)
      call void @rank1(i8** %slot)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  EXPECT_EQ(analysis.getResults().wrong_root_ranks.size(), 1u);
}

TEST_F(MPIAnalysisTest, NonWorldCommunicatorIsNotMarkedAsWorldByClassOrder) {
  const char *source = R"(
    declare i32 @MPI_Bcast(i8*, i32, i32, i32, i8*)

    define i32 @main(i8* %comm) {
    entry:
      call i32 @MPI_Bcast(i8* null, i32 1, i32 0, i32 0, i8* %comm)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  for (const auto &fact : analysis.getCommunicatorFacts()) {
    EXPECT_NE(fact.creation_kind, MPICommunicatorCreationKind::World);
  }
}

TEST_F(MPIAnalysisTest, KnownWorldHandleStillMapsToWorldFact) {
  const char *source = R"(
    @MPI_COMM_WORLD = external global i8
    declare i32 @MPI_Bcast(i8*, i32, i32, i32, i8*)

    define i32 @main() {
    entry:
      %world = load i8, i8* @MPI_COMM_WORLD, align 1
      call i32 @MPI_Bcast(i8* null, i32 1, i32 0, i32 0,
                          i8* @MPI_COMM_WORLD)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  bool saw_world = false;
  for (const auto &fact : analysis.getCommunicatorFacts()) {
    if (fact.creation_kind == MPICommunicatorCreationKind::World) {
      saw_world = true;
    }
  }
  EXPECT_TRUE(saw_world);
}

TEST_F(MPIAnalysisTest, WildcardReceiveWithKnownCommunicatorRemainsMayMatch) {
  const char *source = R"(
    declare i32 @MPI_Send(i8*, i32, i32, i32, i32, i8*)
    declare i32 @MPI_Recv(i8*, i32, i32, i32, i32, i8*, i8*)

    define i32 @main(i8* %comm) {
    entry:
      call i32 @MPI_Send(i8* null, i32 1, i32 0, i32 1, i32 7, i8* %comm)
      call i32 @MPI_Recv(i8* null, i32 1, i32 0, i32 -1, i32 -1, i8* %comm, i8* null)
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

TEST_F(MPIAnalysisTest, FlushLocalDoesNotSuppressPotentialRaces) {
  const char *source = R"(
    @win = global i8 0, align 1
    declare i32 @MPI_Win_create(i8*, i64, i32, i8*, i8*)
    declare i32 @MPI_Win_lock_all(i32, i8*)
    declare i32 @MPI_Win_flush_local_all(i8*)
    declare i32 @MPI_Win_unlock_all(i8*)
    declare i32 @MPI_Put(i8*, i32, i32, i32, i64, i32, i32, i8*)

    define i32 @rank0(i8* %comm) {
    entry:
      call i32 @MPI_Win_create(i8* null, i64 16, i32 4, i8* %comm, i8* @win)
      call i32 @MPI_Win_lock_all(i32 0, i8* @win)
      call i32 @MPI_Put(i8* null, i32 1, i32 0, i32 1, i64 0, i32 1, i32 0, i8* @win)
      call i32 @MPI_Win_flush_local_all(i8* @win)
      call i32 @MPI_Win_unlock_all(i8* @win)
      ret i32 0
    }

    define i32 @rank1(i8* %comm) {
    entry:
      call i32 @MPI_Win_create(i8* null, i64 16, i32 4, i8* %comm, i8* @win)
      call i32 @MPI_Win_lock_all(i32 0, i8* @win)
      call i32 @MPI_Put(i8* null, i32 1, i32 0, i32 1, i64 0, i32 1, i32 0, i8* @win)
      call i32 @MPI_Win_flush_local_all(i8* @win)
      call i32 @MPI_Win_unlock_all(i8* @win)
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

  EXPECT_FALSE(analysis.getResults().rma_races.empty());
}

TEST_F(MPIAnalysisTest, ThreeRankBlockingCycleIsDetected) {
  const char *source = R"(
    declare i32 @MPI_Send(i8*, i32, i32, i32, i32, i8*)
    declare i32 @MPI_Recv(i8*, i32, i32, i32, i32, i8*, i8*)

    define i32 @rank0(i8* %comm) {
    entry:
      call i32 @MPI_Send(i8* null, i32 1, i32 0, i32 1, i32 1, i8* %comm)
      call i32 @MPI_Recv(i8* null, i32 1, i32 0, i32 2, i32 3, i8* %comm, i8* null)
      ret i32 0
    }

    define i32 @rank1(i8* %comm) {
    entry:
      call i32 @MPI_Send(i8* null, i32 1, i32 0, i32 2, i32 2, i8* %comm)
      call i32 @MPI_Recv(i8* null, i32 1, i32 0, i32 0, i32 1, i8* %comm, i8* null)
      ret i32 0
    }

    define i32 @rank2(i8* %comm) {
    entry:
      call i32 @MPI_Send(i8* null, i32 1, i32 0, i32 0, i32 3, i8* %comm)
      call i32 @MPI_Recv(i8* null, i32 1, i32 0, i32 1, i32 2, i8* %comm, i8* null)
      ret i32 0
    }

    define i32 @main(i8* %comm) {
    entry:
      %a = call i32 @rank0(i8* %comm)
      %b = call i32 @rank1(i8* %comm)
      %c = call i32 @rank2(i8* %comm)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  EXPECT_GE(analysis.getResults().potential_deadlocks.size(), 3u);
}

TEST_F(MPIAnalysisTest, SameShapedRMALockEpochsAcrossRanksStillRace) {
  const char *source = R"(
    @win = global i8 0, align 1
    declare i32 @MPI_Win_create(i8*, i64, i32, i8*, i8*)
    declare i32 @MPI_Win_lock_all(i32, i8*)
    declare i32 @MPI_Win_unlock_all(i8*)
    declare i32 @MPI_Put(i8*, i32, i32, i32, i64, i32, i32, i8*)

    define i32 @rank0(i8* %comm) {
    entry:
      call i32 @MPI_Win_create(i8* null, i64 16, i32 4, i8* %comm, i8* @win)
      call i32 @MPI_Win_lock_all(i32 0, i8* @win)
      call i32 @MPI_Put(i8* null, i32 1, i32 0, i32 1, i64 0, i32 1, i32 0, i8* @win)
      call i32 @MPI_Win_unlock_all(i8* @win)
      ret i32 0
    }

    define i32 @rank1(i8* %comm) {
    entry:
      call i32 @MPI_Win_create(i8* null, i64 16, i32 4, i8* %comm, i8* @win)
      call i32 @MPI_Win_lock_all(i32 0, i8* @win)
      call i32 @MPI_Put(i8* null, i32 1, i32 0, i32 1, i64 0, i32 1, i32 0, i8* @win)
      call i32 @MPI_Win_unlock_all(i8* @win)
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

  EXPECT_EQ(analysis.getResults().rma_races.size(), 1u);
}

TEST_F(MPIAnalysisTest, PMPIAndOpenMPINormalizationConfidenceIsExposed) {
  const char *source = R"(
    declare i32 @PMPI_Bcast(i8*, i32, i32, i32, i8*)
    declare i32 @ompi_mpi_bcast(i8*, i32, i32, i32, i8*)

    define i32 @main(i8* %comm) {
    entry:
      call i32 @PMPI_Bcast(i8* null, i32 1, i32 0, i32 0, i8* %comm)
      call i32 @ompi_mpi_bcast(i8* null, i32 1, i32 0, i32 0, i8* %comm)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  bool saw_pmpi = false;
  bool saw_openmpi = false;
  for (const auto &op : analysis.getProcessModel().getAllOperations()) {
    if (op.normalization_confidence == NormalizationConfidence::PMPIWrapper) {
      saw_pmpi = true;
    }
    if (op.normalization_confidence ==
        NormalizationConfidence::KnownOpenMPIForwarder) {
      saw_openmpi = true;
    }
  }
  EXPECT_TRUE(saw_pmpi);
  EXPECT_TRUE(saw_openmpi);
}

TEST_F(MPIAnalysisTest, StructuredDiagnosticsCaptureSemanticRelationAndCode) {
  const char *source = R"(
    declare i32 @MPI_Bcast(i8*, i32, i32, i32, i8*)
    declare i32 @MPI_Gather(i8*, i32, i32, i8*, i32, i32, i32, i8*)

    define i32 @rank0(i8* %comm) {
    entry:
      call i32 @MPI_Bcast(i8* null, i32 1, i32 0, i32 0, i8* %comm)
      ret i32 0
    }

    define i32 @rank1(i8* %comm) {
    entry:
      call i32 @MPI_Gather(i8* null, i32 1, i32 0, i8* null, i32 1, i32 0, i32 0, i8* %comm)
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

  EXPECT_FALSE(analysis.getResults().diagnostics.empty());
  bool saw_protocol_code = false;
  for (const auto &diag : analysis.getResults().diagnostics) {
    if (diag.code == "mpi_collective_protocol_slot" ||
        diag.code == "mpi_collective_protocol_automaton_slot") {
      saw_protocol_code = true;
      break;
    }
  }
  EXPECT_TRUE(saw_protocol_code);
}

TEST_F(MPIAnalysisTest,
       InitThreadRequiredMultipleWithoutProvidedKeepsMustProof) {
  const char *source = R"(
    declare i32 @MPI_Init_thread(i32*, i8***, i32, i32*)
    declare i32 @MPI_Bcast(i8*, i32, i32, i32, i8*)

    define i32 @main(i8* %comm) {
    entry:
      %provided = alloca i32, align 4
      call i32 @MPI_Init_thread(i32* null, i8*** null, i32 3, i32* %provided)
      call i32 @MPI_Bcast(i8* null, i32 1, i32 0, i32 0, i8* %comm)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  EXPECT_TRUE(analysis.getProcessModel().hasInitThreadLevel());
  EXPECT_EQ(analysis.getProcessModel().getRequiredInitThreadLevel(), 3);
  EXPECT_FALSE(analysis.getProcessModel().hasProvidedInitThreadLevel());

  bool saw_protocol_slot = false;
  bool saw_downgraded_collective = false;
  for (const auto &diag : analysis.getResults().diagnostics) {
    if (diag.code == "mpi_collective_protocol_slot") {
      saw_protocol_slot = true;
      EXPECT_EQ(diag.relation.proof, concurrency::ProofStrength::Must);
    }
    if (diag.code == "mpi_collective_protocol_slot_thread_downgrade") {
      saw_downgraded_collective = true;
    }
  }
  EXPECT_TRUE(saw_protocol_slot);
  EXPECT_FALSE(saw_downgraded_collective);
}

TEST_F(MPIAnalysisTest, InitThreadProvidedMultipleDowngradesCollectiveProof) {
  const char *source = R"(
    declare i32 @MPI_Init_thread(i32*, i8***, i32, i32)
    declare i32 @MPI_Bcast(i8*, i32, i32, i32, i8*)

    define i32 @main(i8* %comm) {
    entry:
      call i32 @MPI_Init_thread(i32* null, i8*** null, i32 3, i32 3)
      call i32 @MPI_Bcast(i8* null, i32 1, i32 0, i32 0, i8* %comm)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  EXPECT_TRUE(analysis.getProcessModel().hasProvidedInitThreadLevel());
  EXPECT_EQ(analysis.getProcessModel().getProvidedInitThreadLevel(), 3);

  bool saw_downgraded_collective = false;
  for (const auto &diag : analysis.getResults().diagnostics) {
    if (diag.code == "mpi_collective_protocol_slot_thread_downgrade") {
      saw_downgraded_collective = true;
      EXPECT_EQ(diag.relation.proof, concurrency::ProofStrength::May);
    }
  }
  EXPECT_TRUE(saw_downgraded_collective);
}

TEST_F(MPIAnalysisTest,
       InitThreadRequiredMultipleProvidedFunneledDoesNotDowngrade) {
  const char *source = R"(
    declare i32 @MPI_Init_thread(i32*, i8***, i32, i32)
    declare i32 @MPI_Bcast(i8*, i32, i32, i32, i8*)

    define i32 @main(i8* %comm) {
    entry:
      call i32 @MPI_Init_thread(i32* null, i8*** null, i32 3, i32 1)
      call i32 @MPI_Bcast(i8* null, i32 1, i32 0, i32 0, i8* %comm)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  EXPECT_TRUE(analysis.getProcessModel().hasProvidedInitThreadLevel());
  EXPECT_EQ(analysis.getProcessModel().getProvidedInitThreadLevel(), 1);

  bool saw_protocol_slot = false;
  bool saw_downgraded_collective = false;
  for (const auto &diag : analysis.getResults().diagnostics) {
    if (diag.code == "mpi_collective_protocol_slot") {
      saw_protocol_slot = true;
      EXPECT_EQ(diag.relation.proof, concurrency::ProofStrength::Must);
    }
    if (diag.code == "mpi_collective_protocol_slot_thread_downgrade") {
      saw_downgraded_collective = true;
    }
  }
  EXPECT_TRUE(saw_protocol_slot);
  EXPECT_FALSE(saw_downgraded_collective);
}

TEST_F(MPIAnalysisTest, ExposesParticipantSetsChannelObligationsAndFrontiers) {
  const char *source = R"(
    declare i32 @MPI_Comm_rank(i8*, i32*)
    declare i32 @MPI_Send(i8*, i32, i32, i32, i32, i8*)
    declare i32 @MPI_Recv(i8*, i32, i32, i32, i32, i8*, i8*)
    declare i32 @MPI_Bcast(i8*, i32, i32, i32, i8*)

    define i32 @main(i8* %comm) {
    entry:
      %rank = alloca i32, align 4
      call i32 @MPI_Comm_rank(i8* %comm, i32* %rank)
      %loaded = load i32, i32* %rank, align 4
      %is_root = icmp eq i32 %loaded, 0
      call i32 @MPI_Send(i8* null, i32 1, i32 0, i32 1, i32 7, i8* %comm)
      call i32 @MPI_Recv(i8* null, i32 1, i32 0, i32 1, i32 7, i8* %comm, i8* null)
      br i1 %is_root, label %then, label %else

    then:
      call i32 @MPI_Bcast(i8* null, i32 1, i32 0, i32 0, i8* %comm)
      ret i32 0

    else:
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  EXPECT_FALSE(analysis.getResults().participant_sets.empty());
  ASSERT_FALSE(analysis.getResults().channel_obligations.empty());
  EXPECT_EQ(analysis.getResults().channel_obligations.front().relation.kind,
            concurrency::RelationKind::MatchedCommunication);
  EXPECT_NE(analysis.getResults().channel_obligations.front()
                .sender_set.participant_class_id,
            0u);
  EXPECT_FALSE(analysis.getResults().protocol_frontiers.empty());
  EXPECT_EQ(analysis.getResults().protocol_frontiers.front().relation.kind,
            concurrency::RelationKind::SameCollectiveFrontier);
}

TEST_F(MPIAnalysisTest, NullCommunicatorPointToPointProducesStructuredModelGap) {
  const char *source = R"(
    declare i32 @MPI_Send(i8*, i32, i32, i32, i32, i8*)
    declare i32 @MPI_Recv(i8*, i32, i32, i32, i32, i8*, i8*)

    define i32 @main(i8* %comm) {
    entry:
      call i32 @MPI_Send(i8* null, i32 1, i32 0, i32 1, i32 7, i8* %comm)
      call i32 @MPI_Recv(i8* null, i32 1, i32 0, i32 -1, i32 -2, i8* %comm, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  ASSERT_FALSE(analysis.getResults().model_gaps.empty());
  bool saw_channel_gap = false;
  for (const auto &gap : analysis.getResults().model_gaps) {
    if (gap.domain == MPIModelGapDomain::PointToPoint) {
      saw_channel_gap = true;
    }
  }
  EXPECT_TRUE(saw_channel_gap);
}

TEST_F(MPIAnalysisTest, FlushLocalProducesLocalOnlySynchronizationFact) {
  const char *source = R"(
    @win = global i8 0

    declare i32 @MPI_Win_create(i8*, i64, i32, i8*, i8*)
    declare i32 @MPI_Win_lock_all(i32, i8*)
    declare i32 @MPI_Put(i8*, i32, i32, i32, i64, i32, i32, i8*)
    declare i32 @MPI_Win_flush_local_all(i8*)
    declare i32 @MPI_Win_unlock_all(i8*)

    define i32 @main(i8* %comm) {
    entry:
      call i32 @MPI_Win_create(i8* null, i64 16, i32 4, i8* %comm, i8* @win)
      call i32 @MPI_Win_lock_all(i32 0, i8* @win)
      call i32 @MPI_Put(i8* null, i32 1, i32 0, i32 1, i64 0, i32 1, i32 0, i8* @win)
      call i32 @MPI_Win_flush_local_all(i8* @win)
      call i32 @MPI_Win_unlock_all(i8* @win)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  bool saw_local_completion = false;
  for (const auto &fact : analysis.getResults().rma_synchronization_facts) {
    if (fact.completion == MPIRMACompletionStrength::Local) {
      saw_local_completion = true;
      EXPECT_EQ(fact.relation.kind,
                concurrency::RelationKind::LocalOnlySynchronizationCompletion);
    }
  }
  EXPECT_TRUE(saw_local_completion);
}

TEST_F(MPIAnalysisTest, AbstractStateExposesCommunicatorFactsAndSummaries) {
  const char *source = R"(
    declare i32 @MPI_Comm_split(i8*, i32, i32, i8**)
    declare i32 @MPI_Bcast(i8*, i32, i32, i32, i8*)

    define void @worker(i8* %comm) {
    entry:
      call i32 @MPI_Bcast(i8* null, i32 1, i32 0, i32 0, i8* %comm)
      ret void
    }

    define i32 @main(i8* %world) {
    entry:
      %sub = alloca i8*, align 8
      call i32 @MPI_Comm_split(i8* %world, i32 0, i32 7, i8** %sub)
      %child = load i8*, i8** %sub, align 8
      call void @worker(i8* %child)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  const auto &communicators = analysis.getCommunicatorFacts();
  ASSERT_FALSE(communicators.empty());
  bool saw_split = false;
  for (const auto &fact : communicators) {
    if (fact.creation_kind == MPICommunicatorCreationKind::Split) {
      saw_split = true;
      EXPECT_NE(fact.communicator_class_id, 0u);
    }
  }
  EXPECT_TRUE(saw_split);

  const auto &summaries = analysis.getFunctionSummaries();
  ASSERT_EQ(summaries.size(), 2u);
  const auto main_summary = std::find_if(
      summaries.begin(), summaries.end(),
      [](const MPIFunctionSummary &summary) {
        return summary.function && summary.function->getName() == "main";
      });
  ASSERT_NE(main_summary, summaries.end());
  EXPECT_GT(main_summary->expanded_operation_indices.size(),
            main_summary->direct_operation_indices.size());
  EXPECT_TRUE(main_summary->reaches_fixed_point);
}

TEST_F(MPIAnalysisTest, WinSyncRemainsLocalOnlyCompletion) {
  const char *source = R"(
    @win = global i8 0

    declare i32 @MPI_Win_create(i8*, i64, i32, i8*, i8*)
    declare i32 @MPI_Win_lock(i32, i32, i32, i8*)
    declare i32 @MPI_Put(i8*, i32, i32, i32, i64, i32, i32, i8*)
    declare i32 @MPI_Win_sync(i8*)
    declare i32 @MPI_Win_unlock(i32, i8*)

    define i32 @main(i8* %comm) {
    entry:
      call i32 @MPI_Win_create(i8* null, i64 16, i32 4, i8* %comm, i8* @win)
      call i32 @MPI_Win_lock(i32 0, i32 1, i32 0, i8* @win)
      call i32 @MPI_Put(i8* null, i32 1, i32 0, i32 1, i64 0, i32 1, i32 0,
                        i8* @win)
      call i32 @MPI_Win_sync(i8* @win)
      call i32 @MPI_Win_unlock(i32 1, i8* @win)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  bool saw_local_completion = false;
  bool saw_remote_completion = false;
  for (const auto &fact : analysis.getResults().rma_synchronization_facts) {
    if (fact.completion == MPIRMACompletionStrength::Local) {
      saw_local_completion = true;
      EXPECT_EQ(fact.relation.kind,
                concurrency::RelationKind::LocalOnlySynchronizationCompletion);
    } else if (fact.completion == MPIRMACompletionStrength::Remote) {
      saw_remote_completion = true;
    }
  }
  EXPECT_TRUE(saw_local_completion);
  EXPECT_FALSE(saw_remote_completion);
}

TEST_F(MPIAnalysisTest, AbstractStateExposesRequestFactsAndChannelAutomata) {
  const char *source = R"(
    declare i32 @MPI_Isend(i8*, i32, i32, i32, i32, i8*, i8*)
    declare i32 @MPI_Wait(i8*, i8*)
    declare i32 @MPI_Send(i8*, i32, i32, i32, i32, i8*)
    declare i32 @MPI_Recv(i8*, i32, i32, i32, i32, i8*, i8*)

    define i32 @main(i8* %comm) {
    entry:
      %req = alloca i8, align 1
      call i32 @MPI_Isend(i8* null, i32 1, i32 0, i32 1, i32 7, i8* %comm, i8* %req)
      call i32 @MPI_Wait(i8* %req, i8* null)
      call i32 @MPI_Send(i8* null, i32 1, i32 0, i32 0, i32 9, i8* %comm)
      call i32 @MPI_Recv(i8* null, i32 1, i32 0, i32 0, i32 9, i8* %comm, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  const auto &request_facts = analysis.getRequestFacts();
  ASSERT_EQ(request_facts.size(), 1u);
  EXPECT_EQ(request_facts.front().kind, MPIRequestFactKind::PointToPoint);
  EXPECT_EQ(request_facts.front().state, MPIRequestState::MustComplete);
  EXPECT_EQ(request_facts.front().relation.kind,
            concurrency::RelationKind::MPIRequestCompletion);

  const auto &channel_automata = analysis.getChannelAutomata();
  ASSERT_FALSE(channel_automata.empty());
  const auto automaton = std::find_if(
      channel_automata.begin(), channel_automata.end(),
      [](const MPIChannelAutomaton &state) {
        return !state.obligations.empty() && !state.transitions.empty();
      });
  ASSERT_NE(automaton, channel_automata.end());
  EXPECT_GE(automaton->posted_receive_count, 1u);
  EXPECT_GE(automaton->transitions.size(), 2u);
}

TEST_F(MPIAnalysisTest, RequestSetFactsCaptureWaitallAndWaitanyScopes) {
  const char *source = R"(
    declare i32 @MPI_Isend(i8*, i32, i32, i32, i32, i8*, i8*)
    declare i32 @MPI_Waitall(i32, i8**, i8*)
    declare i32 @MPI_Waitany(i32, i8**, i32*, i8*)

    define i32 @main(i8* %comm) {
    entry:
      %req1 = alloca i8, align 1
      %req2 = alloca i8, align 1
      %reqs = alloca [2 x i8*], align 8
      %slot0 = getelementptr inbounds [2 x i8*], [2 x i8*]* %reqs, i64 0, i64 0
      %slot1 = getelementptr inbounds [2 x i8*], [2 x i8*]* %reqs, i64 0, i64 1
      %idx = alloca i32, align 4
      store i8* %req1, i8** %slot0, align 8
      store i8* %req2, i8** %slot1, align 8
      call i32 @MPI_Isend(i8* null, i32 1, i32 0, i32 0, i32 7, i8* %comm, i8* %req1)
      call i32 @MPI_Isend(i8* null, i32 1, i32 0, i32 0, i32 8, i8* %comm, i8* %req2)
      call i32 @MPI_Waitall(i32 2, i8** %slot0, i8* null)
      call i32 @MPI_Waitany(i32 2, i8** %slot0, i32* %idx, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  const auto &request_sets = analysis.getRequestSetFacts();
  bool saw_waitall = false;
  bool saw_waitany = false;
  for (const auto &fact : request_sets) {
    if (fact.provenance == "mpi_request_set_complete") {
      saw_waitall = saw_waitall ||
                    (fact.completion_scope ==
                         MPIRequestCompletionScopeKind::AllOfSet &&
                     fact.state == MPIRequestState::MustComplete &&
                     fact.requests.size() == 2);
    }
    if (fact.provenance == "mpi_request_set_may_complete") {
      saw_waitany = saw_waitany ||
                    (fact.completion_scope ==
                         MPIRequestCompletionScopeKind::OneOfSet &&
                     fact.state == MPIRequestState::MayComplete &&
                     fact.requests.size() == 2);
    }
  }
  EXPECT_TRUE(saw_waitall);
  EXPECT_TRUE(saw_waitany);
}

TEST_F(MPIAnalysisTest,
       RequestFactsAndChannelAutomataExposeRequestSetAndChannelIDs) {
  const char *source = R"(
    declare i32 @MPI_Isend(i8*, i32, i32, i32, i32, i8*, i8*)
    declare i32 @MPI_Recv(i8*, i32, i32, i32, i32, i8*, i8*)

    define i32 @main(i8* %comm) {
    entry:
      %req = alloca i8, align 1
      call i32 @MPI_Isend(i8* null, i32 1, i32 0, i32 0, i32 7, i8* %comm, i8* %req)
      call i32 @MPI_Recv(i8* null, i32 1, i32 0, i32 0, i32 7, i8* %comm, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  const auto &request_facts = analysis.getRequestFacts();
  ASSERT_EQ(request_facts.size(), 1u);
  EXPECT_NE(request_facts.front().channel_class_id, 0u);
  EXPECT_NE(request_facts.front().request_set_id, 0u);

  const auto &request_sets = analysis.getRequestSetFacts();
  ASSERT_FALSE(request_sets.empty());
  auto request_set_it =
      std::find_if(request_sets.begin(), request_sets.end(),
                   [&](const MPIRequestSetFact &fact) {
                     return fact.request_set_id == request_facts.front().request_set_id;
                   });
  ASSERT_NE(request_set_it, request_sets.end());
  EXPECT_NE(request_set_it->channel_class_id, 0u);

  const auto &channel_automata = analysis.getChannelAutomata();
  auto automaton = std::find_if(
      channel_automata.begin(), channel_automata.end(),
      [&](const MPIChannelAutomaton &state) {
        return !state.unresolved_completion_request_set_ids.empty();
      });
  ASSERT_NE(automaton, channel_automata.end());
  EXPECT_FALSE(automaton->posted_send_obligation_ids.empty());
  EXPECT_FALSE(automaton->posted_receive_obligation_ids.empty());
  EXPECT_NE(std::find(automaton->unresolved_completion_request_set_ids.begin(),
                      automaton->unresolved_completion_request_set_ids.end(),
                      request_facts.front().request_set_id),
            automaton->unresolved_completion_request_set_ids.end());
}

TEST_F(MPIAnalysisTest, ChannelAutomatonTracksUniqueMatchTransitions) {
  const char *source = R"(
    declare i32 @MPI_Send(i8*, i32, i32, i32, i32, i8*)
    declare i32 @MPI_Recv(i8*, i32, i32, i32, i32, i8*, i8*)

    define i32 @main(i8* %comm) {
    entry:
      call i32 @MPI_Send(i8* null, i32 1, i32 0, i32 0, i32 7, i8* %comm)
      call i32 @MPI_Recv(i8* null, i32 1, i32 0, i32 0, i32 7, i8* %comm, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  const auto &channel_automata = analysis.getChannelAutomata();
  ASSERT_FALSE(channel_automata.empty());
  const MPIChannelAutomaton &automaton = channel_automata.front();
  EXPECT_EQ(automaton.ambiguity_state, MPIChannelAutomaton::AmbiguityState::Unique);
  EXPECT_EQ(automaton.matched_endpoint_pairs.size(), 1u);

  size_t unique_matches = 0;
  size_t post_sends = 0;
  size_t post_receives = 0;
  for (const MPIChannelTransition &transition : automaton.transitions) {
    if (transition.kind == MPIChannelTransition::Kind::UniqueMatch) {
      ++unique_matches;
      EXPECT_EQ(transition.proof, MPICommunicationMatch::MustMatch);
    } else if (transition.kind == MPIChannelTransition::Kind::PostSend) {
      ++post_sends;
    } else if (transition.kind == MPIChannelTransition::Kind::PostReceive) {
      ++post_receives;
    }
  }
  EXPECT_EQ(unique_matches, 2u);
  EXPECT_EQ(post_sends, 1u);
  EXPECT_EQ(post_receives, 1u);
}

TEST_F(MPIAnalysisTest, ChannelAutomatonTracksNonUniqueCandidates) {
  const char *source = R"(
    declare i32 @MPI_Send(i8*, i32, i32, i32, i32, i8*)
    declare i32 @MPI_Recv(i8*, i32, i32, i32, i32, i8*, i8*)

    define i32 @main(i8* %comm) {
    entry:
      call i32 @MPI_Send(i8* null, i32 1, i32 0, i32 0, i32 7, i8* %comm)
      call i32 @MPI_Recv(i8* null, i32 1, i32 0, i32 -1, i32 7, i8* %comm, i8* null)
      call i32 @MPI_Recv(i8* null, i32 1, i32 0, i32 -1, i32 7, i8* %comm, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  bool saw_nonunique = false;
  for (const MPIChannelAutomaton &automaton : analysis.getChannelAutomata()) {
    saw_nonunique = saw_nonunique ||
                    automaton.ambiguity_state == MPIChannelAutomaton::AmbiguityState::NonUnique;
    bool has_unique_match = false;
    bool has_candidate_match = false;
    for (const MPIChannelTransition &transition : automaton.transitions) {
      has_unique_match = has_unique_match ||
                         transition.kind == MPIChannelTransition::Kind::UniqueMatch;
      has_candidate_match = has_candidate_match ||
                            transition.kind == MPIChannelTransition::Kind::CandidateMatch;
    }
    if (automaton.ambiguity_state == MPIChannelAutomaton::AmbiguityState::NonUnique) {
      EXPECT_FALSE(has_unique_match);
      EXPECT_TRUE(has_candidate_match);
    }
  }
  EXPECT_TRUE(saw_nonunique);
}

TEST_F(MPIAnalysisTest, FunctionSummariesComposeChannelAndRequestEffects) {
  const char *source = R"(
    declare i32 @MPI_Isend(i8*, i32, i32, i32, i32, i8*, i8*)
    declare i32 @MPI_Wait(i8*, i8*)
    declare i32 @MPI_Recv(i8*, i32, i32, i32, i32, i8*, i8*)

    define void @helper(i8* %comm, i8* %req) {
    entry:
      call i32 @MPI_Isend(i8* null, i32 1, i32 0, i32 0, i32 7, i8* %comm, i8* %req)
      call i32 @MPI_Wait(i8* %req, i8* null)
      ret void
    }

    define void @peer(i8* %comm) {
    entry:
      call i32 @MPI_Recv(i8* null, i32 1, i32 0, i32 0, i32 7, i8* %comm, i8* null)
      ret void
    }

    define i32 @main(i8* %comm) {
    entry:
      %req = alloca i8, align 1
      call void @helper(i8* %comm, i8* %req)
      call void @peer(i8* %comm)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  const auto &summaries = analysis.getFunctionSummaries();
  ASSERT_GE(summaries.size(), 3u);
  const auto helper_summary = std::find_if(
      summaries.begin(), summaries.end(), [](const MPIFunctionSummary &summary) {
        return summary.function && summary.function->getName() == "helper";
      });
  const auto main_summary = std::find_if(
      summaries.begin(), summaries.end(), [](const MPIFunctionSummary &summary) {
        return summary.function && summary.function->getName() == "main";
      });
  ASSERT_NE(helper_summary, summaries.end());
  ASSERT_NE(main_summary, summaries.end());

  EXPECT_FALSE(helper_summary->emitted_send_endpoint_ids.empty());
  EXPECT_FALSE(helper_summary->created_request_set_ids.empty());
  EXPECT_FALSE(helper_summary->discharged_request_set_ids.empty());
  EXPECT_FALSE(helper_summary->blocking_request_set_ids.empty());
  EXPECT_FALSE(main_summary->emitted_send_endpoint_ids.empty());
  EXPECT_FALSE(main_summary->created_request_set_ids.empty());
  EXPECT_FALSE(main_summary->discharged_request_set_ids.empty());
  EXPECT_EQ(helper_summary->emitted_send_endpoint_ids,
            main_summary->emitted_send_endpoint_ids);
  EXPECT_FALSE(helper_summary->unresolved_indirect_call_effect);
  EXPECT_FALSE(main_summary->unresolved_indirect_call_effect);
}

TEST_F(MPIAnalysisTest, FunctionSummaryMarksIndirectCallEffectsUnresolved) {
  const char *source = R"(
    declare i32 @MPI_Barrier(i8*)

    define void @target(i8* %comm) {
    entry:
      call i32 @MPI_Barrier(i8* %comm)
      ret void
    }

    define i32 @main(i8* %comm, void (i8*)* %fn) {
    entry:
      call void %fn(i8* %comm)
      call i32 @MPI_Barrier(i8* %comm)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  const auto &summaries = analysis.getFunctionSummaries();
  auto main_summary = std::find_if(
      summaries.begin(), summaries.end(), [](const MPIFunctionSummary &summary) {
        return summary.function && summary.function->getName() == "main";
      });
  ASSERT_NE(main_summary, summaries.end());
  EXPECT_TRUE(main_summary->unresolved_indirect_call_effect);

  bool saw_summary_gap = false;
  for (const MPIModelGap &gap : analysis.getResults().model_gaps) {
    if (gap.code == "mpi_summary_indirect_call") {
      saw_summary_gap = true;
    }
  }
  EXPECT_TRUE(saw_summary_gap);
}

TEST_F(MPIAnalysisTest, FunctionSummaryTracksOutstandingChannelAndRequestState) {
  const char *source = R"(
    declare i32 @MPI_Isend(i8*, i32, i32, i32, i32, i8*, i8*)
    declare i32 @MPI_Recv(i8*, i32, i32, i32, i32, i8*, i8*)

    define i32 @worker(i8* %comm) {
    entry:
      %req = alloca i8, align 1
      call i32 @MPI_Isend(i8* null, i32 1, i32 0, i32 0, i32 7, i8* %comm, i8* %req)
      call i32 @MPI_Recv(i8* null, i32 1, i32 0, i32 -1, i32 7, i8* %comm, i8* null)
      ret i32 0
    }

    define i32 @main(i8* %comm) {
    entry:
      %x = call i32 @worker(i8* %comm)
      ret i32 %x
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  const auto &summaries = analysis.getFunctionSummaries();
  auto worker_summary = std::find_if(
      summaries.begin(), summaries.end(), [](const MPIFunctionSummary &summary) {
        return summary.function && summary.function->getName() == "worker";
      });
  ASSERT_NE(worker_summary, summaries.end());
  EXPECT_FALSE(worker_summary->emitted_send_endpoint_ids.empty());
  EXPECT_FALSE(worker_summary->created_request_set_ids.empty());
  EXPECT_FALSE(worker_summary->outstanding_send_endpoint_ids.empty());
  EXPECT_FALSE(worker_summary->outstanding_receive_endpoint_ids.empty());
  EXPECT_FALSE(worker_summary->outstanding_request_set_ids.empty());
  EXPECT_FALSE(worker_summary->unresolved_channel_class_ids.empty());
}

TEST_F(MPIAnalysisTest, CollectiveSummaryTracksEnteredSlotsAndUnresolvedState) {
  const char *source = R"(
    declare i32 @MPI_Comm_rank(i8*, i32*)
    declare i32 @MPI_Bcast(i8*, i32, i32, i32, i8*)

    define void @helper(i8* %comm) {
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
      ret void
    }

    define i32 @main(i8* %comm) {
    entry:
      call void @helper(i8* %comm)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  const auto &summaries = analysis.getFunctionSummaries();
  auto helper_summary = std::find_if(
      summaries.begin(), summaries.end(), [](const MPIFunctionSummary &summary) {
        return summary.function && summary.function->getName() == "helper";
      });
  ASSERT_NE(helper_summary, summaries.end());
  EXPECT_FALSE(helper_summary->collective_call_operation_indices.empty());
  EXPECT_FALSE(helper_summary->entered_collective_protocol_slots.empty());
  EXPECT_TRUE(helper_summary->unresolved_collective_summary_effect);

  bool saw_collective_gap = false;
  for (const MPIModelGap &gap : analysis.getResults().model_gaps) {
    if (gap.code == "mpi_collective_summary_unresolved" ||
        gap.code == "mpi_collective_recursive_summary_unresolved") {
      saw_collective_gap = true;
    }
  }
  EXPECT_TRUE(saw_collective_gap);
}

TEST_F(MPIAnalysisTest, ValidationKeepsRequestFactsAndChannelAutomataConsistent) {
  const char *source = R"(
    declare i32 @MPI_Isend(i8*, i32, i32, i32, i32, i8*, i8*)
    declare i32 @MPI_Irecv(i8*, i32, i32, i32, i32, i8*, i8*)
    declare i32 @MPI_Waitall(i32, i8**, i8*)

    define i32 @main(i8* %comm) {
    entry:
      %req1 = alloca i8, align 1
      %req2 = alloca i8, align 1
      %reqs = alloca [2 x i8*], align 8
      %slot0 = getelementptr inbounds [2 x i8*], [2 x i8*]* %reqs, i64 0, i64 0
      %slot1 = getelementptr inbounds [2 x i8*], [2 x i8*]* %reqs, i64 0, i64 1
      store i8* %req1, i8** %slot0, align 8
      store i8* %req2, i8** %slot1, align 8
      call i32 @MPI_Isend(i8* null, i32 1, i32 0, i32 0, i32 7, i8* %comm, i8* %req1)
      call i32 @MPI_Irecv(i8* null, i32 1, i32 0, i32 0, i32 7, i8* %comm, i8* %req2)
      call i32 @MPI_Waitall(i32 2, i8** %slot0, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  std::set<size_t> request_set_ids;
  for (const auto &request_set : analysis.getRequestSetFacts()) {
    request_set_ids.insert(request_set.request_set_id);
  }
  std::set<size_t> channel_ids;
  for (const auto &automaton : analysis.getChannelAutomata()) {
    channel_ids.insert(automaton.channel_class_id);
  }

  for (const MPIRequestFact &fact : analysis.getRequestFacts()) {
    if (fact.request_set_id != 0) {
      EXPECT_NE(request_set_ids.count(fact.request_set_id), 0u);
    }
    if (fact.channel_class_id != 0) {
      EXPECT_NE(channel_ids.count(fact.channel_class_id), 0u);
    }
  }
}

TEST_F(MPIAnalysisTest, ValidationKeepsChannelObligationsAndAutomataConsistent) {
  const char *source = R"(
    declare i32 @MPI_Send(i8*, i32, i32, i32, i32, i8*)
    declare i32 @MPI_Recv(i8*, i32, i32, i32, i32, i8*, i8*)

    define i32 @rank0(i8* %comm) {
    entry:
      call i32 @MPI_Send(i8* null, i32 1, i32 0, i32 1, i32 7, i8* %comm)
      ret i32 0
    }

    define i32 @rank1(i8* %comm) {
    entry:
      call i32 @MPI_Recv(i8* null, i32 1, i32 0, i32 0, i32 7, i8* %comm, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  std::set<std::pair<size_t, size_t>> matched_pairs;
  for (const MPIChannelObligation &obligation : analysis.getResults().channel_obligations) {
    EXPECT_NE(obligation.sender_obligation_id, 0u);
    EXPECT_NE(obligation.receiver_obligation_id, 0u);
    matched_pairs.emplace(obligation.sender_obligation_id, obligation.receiver_obligation_id);
  }

  for (const MPIChannelAutomaton &automaton : analysis.getChannelAutomata()) {
    for (const auto &pair : automaton.matched_endpoint_pairs) {
      EXPECT_NE(matched_pairs.count(pair), 0u);
    }
  }
}

TEST_F(MPIAnalysisTest, ValidationKeepsSummaryReferencesConsistent) {
  const char *source = R"(
    declare i32 @MPI_Comm_rank(i8*, i32*)
    declare i32 @MPI_Bcast(i8*, i32, i32, i32, i8*)
    declare i32 @MPI_Isend(i8*, i32, i32, i32, i32, i8*, i8*)
    declare i32 @MPI_Wait(i8*, i8*)

    define void @helper(i8* %comm, i8* %req) {
    entry:
      %rank = alloca i32, align 4
      call i32 @MPI_Comm_rank(i8* %comm, i32* %rank)
      %loaded = load i32, i32* %rank, align 4
      %root = icmp eq i32 %loaded, 0
      br i1 %root, label %then, label %join

    then:
      call i32 @MPI_Bcast(i8* null, i32 1, i32 0, i32 0, i8* %comm)
      br label %join

    join:
      call i32 @MPI_Isend(i8* null, i32 1, i32 0, i32 0, i32 7, i8* %comm, i8* %req)
      call i32 @MPI_Wait(i8* %req, i8* null)
      ret void
    }

    define i32 @main(i8* %comm) {
    entry:
      %req = alloca i8, align 1
      call void @helper(i8* %comm, i8* %req)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  std::set<size_t> request_set_ids;
  for (const auto &request_set : analysis.getRequestSetFacts()) {
    request_set_ids.insert(request_set.request_set_id);
  }
  std::set<size_t> frontier_ids;
  for (const auto &frontier : analysis.getResults().protocol_frontiers) {
    frontier_ids.insert(frontier.frontier_id);
  }

  for (const MPIFunctionSummary &summary : analysis.getFunctionSummaries()) {
    for (size_t id : summary.outstanding_request_set_ids) {
      EXPECT_NE(request_set_ids.count(id), 0u);
    }
    for (size_t id : summary.outstanding_collective_frontier_ids) {
      EXPECT_NE(frontier_ids.count(id), 0u);
    }
  }
}

TEST_F(MPIAnalysisTest, PrintResultsIncludesValidationCounters) {
  const char *source = R"(
    declare i32 @MPI_Isend(i8*, i32, i32, i32, i32, i8*, i8*)
    declare i32 @MPI_Testany(i32, i8**, i32*, i32*, i8*)

    define i32 @main(i8* %comm) {
    entry:
      %req1 = alloca i8, align 1
      %req2 = alloca i8, align 1
      %reqs = alloca [2 x i8*], align 8
      %slot0 = getelementptr inbounds [2 x i8*], [2 x i8*]* %reqs, i64 0, i64 0
      %slot1 = getelementptr inbounds [2 x i8*], [2 x i8*]* %reqs, i64 0, i64 1
      %index = alloca i32, align 4
      %flag = alloca i32, align 4
      store i8* %req1, i8** %slot0, align 8
      store i8* %req2, i8** %slot1, align 8
      store i32 1, i32* %flag, align 4
      call i32 @MPI_Isend(i8* null, i32 1, i32 0, i32 1, i32 7, i8* %comm, i8* %req1)
      call i32 @MPI_Isend(i8* null, i32 1, i32 0, i32 2, i32 8, i8* %comm, i8* %req2)
      call i32 @MPI_Testany(i32 2, i8** %slot0, i32* %index, i32* %flag, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  std::string output;
  raw_string_ostream os(output);
  analysis.printResults(os);
  os.flush();

  EXPECT_NE(output.find("Request sets tracked:"), std::string::npos);
  EXPECT_NE(output.find("Non-unique channel automata:"), std::string::npos);
  EXPECT_NE(output.find("Unresolved-identity channel automata:"),
            std::string::npos);
  EXPECT_NE(output.find("Unresolved request sets:"), std::string::npos);
  EXPECT_NE(output.find("Unresolved collective summaries:"),
            std::string::npos);
}

TEST_F(MPIAnalysisTest, AbstractStateExposesProtocolAndEpochFacts) {
  const char *source = R"(
    declare i32 @MPI_Barrier(i8*)
    declare i32 @MPI_Win_create(i8*, i32, i32, i8*, i8*)
    declare i32 @MPI_Win_lock(i32, i32, i32, i8*)
    declare i32 @MPI_Put(i8*, i32, i32, i32, i32, i32, i32, i8*)
    declare i32 @MPI_Win_flush(i32, i8*)
    declare i32 @MPI_Win_unlock(i32, i8*)

    define i32 @main(i8* %comm) {
    entry:
      %win = alloca i8, align 1
      call i32 @MPI_Barrier(i8* %comm)
      call i32 @MPI_Win_create(i8* null, i32 8, i32 4, i8* %comm, i8* %win)
      call i32 @MPI_Win_lock(i32 0, i32 1, i32 0, i8* %win)
      call i32 @MPI_Put(i8* null, i32 1, i32 0, i32 1, i32 0, i32 1, i32 0, i8* %win)
      call i32 @MPI_Win_flush(i32 1, i8* %win)
      call i32 @MPI_Win_unlock(i32 1, i8* %win)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  const auto &protocol_states = analysis.getCollectiveProtocolStates();
  ASSERT_FALSE(protocol_states.empty());
  EXPECT_EQ(protocol_states.front().relation.kind,
            concurrency::RelationKind::MPICollectiveParticipation);
  EXPECT_FALSE(protocol_states.front().operations.empty());

  const auto &epoch_facts = analysis.getRMAEpochFacts();
  ASSERT_FALSE(epoch_facts.empty());
  bool saw_epoch = false;
  for (const auto &fact : epoch_facts) {
    if (fact.epoch_id != 0 && !fact.operations.empty()) {
      saw_epoch = true;
    }
  }
  EXPECT_TRUE(saw_epoch);
}

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
