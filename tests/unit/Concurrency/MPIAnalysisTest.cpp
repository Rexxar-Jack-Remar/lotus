#include "Analysis/Concurrency/MPI/MPIAnalysis.h"

#include "Analysis/Concurrency/MPI/MPISemantics.h"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <vector>

#include <gtest/gtest.h>
#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;
using namespace mpi;

class MPIAnalysisTest : public ::testing::Test {
protected:
  LLVMContext context;

  std::unique_ptr<Module> parseModule(const char *source) {
    SMDiagnostic err;
    auto module = parseAssemblyString(source, err, context);
    if (!module) {
      err.print("MPIAnalysisTest", errs());
    }
    return module;
  }
};

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
    declare i32 @MPI_Send(i8*, i32, i32, i32, i32, i8*)
    declare i32 @MPI_Recv(i8*, i32, i32, i32, i32, i8*, i8*)

    define i32 @main(i8* %comm) {
    entry:
      call i32 @MPI_Send(i8* null, i32 1, i32 0, i32 0, i32 7, i8* %comm)
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
  EXPECT_NE(collectives[0].communicator_subgroup_id, 0u);
  EXPECT_NE(collectives[1].communicator_subgroup_id, 0u);
  EXPECT_NE(collectives[0].communicator_subgroup_id,
            collectives[1].communicator_subgroup_id);
  EXPECT_EQ(collectives[0].protocol_sequence_id, 0u);
  EXPECT_EQ(collectives[1].protocol_sequence_id, 0u);
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
  EXPECT_EQ(automaton.participant_slots.size(), 2u);
  EXPECT_EQ(automaton.slots.size(), 2u);
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
      call i32 @MPI_Fetch_and_op(i8* null, i8* null, i32 0, i32 3, i64 0, i32 0, i8* %win)
      call i32 @MPI_Compare_and_swap(i8* null, i8* null, i8* null, i32 0, i32 4, i64 8, i8* %win)
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
      call i32 @PMPI_Fetch_and_op(i8* null, i8* null, i32 0, i32 5, i64 8, i32 0, i8* %win)
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

  EXPECT_TRUE(analysis.getResults().unsynchronized_rma.empty());
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

  EXPECT_TRUE(analysis.getResults().unsynchronized_rma.empty());
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
  EXPECT_TRUE(analysis.getResults().unsynchronized_rma.empty());
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
    if (gap.code == "mpi_channel_unknown" || gap.code == "mpi_channel_partial") {
      saw_channel_gap = true;
      EXPECT_EQ(gap.domain, MPIModelGapDomain::PointToPoint);
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
