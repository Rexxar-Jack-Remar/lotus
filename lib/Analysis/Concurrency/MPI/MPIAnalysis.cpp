/**
 * @file MPIAnalysis.cpp
 * @brief MPI Program Analysis Coordinator Implementation
 *
 * @author Lotus Analysis Framework
 * @date 2026
 */

#include "Analysis/Concurrency/MPI/MPIAnalysis.h"

#include "Analysis/Concurrency/MPI/MPICollectiveAnalysis.h"
#include "Analysis/Concurrency/MPI/MPIOperation.h"
#include "Analysis/Concurrency/MPI/MPIProcessModel.h"
#include "Analysis/Concurrency/MPI/MPIRMAAnalysis.h"

#include <unordered_map>
#include <vector>

#include <llvm/IR/Instruction.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;

namespace mpi {

size_t MPIAnalysis::getProtocolDiagnosticCount(StringRef key) const {
  const auto &diagnostics = collective_analysis_.getProtocolDiagnostics();
  auto it = diagnostics.find(key.str());
  return it != diagnostics.end() ? it->second : 0;
}

size_t MPIAnalysis::getOperationCount(MPIOpKind kind) const {
  const auto &counts = process_model_.getOperationKindCounts();
  auto it = counts.find(kind);
  return it != counts.end() ? it->second : 0;
}

size_t MPIAnalysis::getTrackedWindowCount() const {
  return rma_analysis_.getTrackedWindowCount();
}

void MPIAnalysis::runAnalysis() {
  process_model_.analyzeModule();
  collective_analysis_.analyzeCollectives();
  rma_analysis_.analyzeRMA();

  results_.orphaned_requests = process_model_.findOrphanedNonBlockingOps();
  results_.potential_deadlocks = process_model_.findPotentialDeadlocks();
  results_.mismatched_collectives =
      collective_analysis_.findMismatchedCollectives();
  results_.conditional_collectives =
      collective_analysis_.findConditionalCollectives();
  results_.unsynchronized_rma = rma_analysis_.findUnsynchronizedRMAOps();
  results_.rma_races = rma_analysis_.findRMARaces();
  results_.leaked_windows = rma_analysis_.findLeakedWindows();

  const MPIOperation *first_finalize = nullptr;
  bool has_init = false;
  for (const MPIOperation &op : process_model_.getAllOperations()) {
    if (op.kind == MPIOpKind::FINALIZE) {
      if (first_finalize) {
        results_.double_finalize.push_back(op.inst);
      } else {
        first_finalize = &op;
      }
    }
    if (op.kind == MPIOpKind::INIT) {
      has_init = true;
    }
  }

  if (has_init && !first_finalize) {
    results_.missing_finalize = true;
  }

  results_.tag_mismatches = process_model_.findTagMismatches();
  results_.count_datatype_mismatches =
      process_model_.findCountDatatypeMismatches();
  results_.rank_out_of_bounds = process_model_.findRankOutOfBounds();
  results_.persistent_request_leaks =
      process_model_.findPersistentRequestLeaks();
  results_.wrong_root_ranks = collective_analysis_.findWrongRootRanks();
  results_.cancel_without_wait = process_model_.findCancelWithoutWait();
  results_.buffer_overlaps = process_model_.findBufferOverlaps();
  results_.wildcard_in_collective = process_model_.findWildcardInCollective();
  results_.in_place_conflicts = process_model_.findInPlaceConflicts();
  results_.null_handles = process_model_.findNullHandles();
  results_.negative_root = process_model_.findNegativeRoot();
  results_.invalid_tags = process_model_.findInvalidTags();
  results_.invalid_ranks = process_model_.findInvalidRanks();
  results_.type_size_mismatches = process_model_.findTypeSizeMismatches();
  results_.destroy_null_comm = process_model_.findDestroyNullComm();
  results_.request_free_after_wait = process_model_.findRequestFreeAfterWait();
  results_.in_place_wrong_op = process_model_.findInPlaceWrongOp();
  results_.invalid_rma_transitions =
      rma_analysis_.findInvalidEpochTransitions();
  results_.use_after_free_windows = rma_analysis_.findUseAfterFreeWindows();
  results_.double_window_free = rma_analysis_.findDoubleWindowFree();
}

void MPIAnalysis::printResults(raw_ostream &OS) const {
  const auto &operations = process_model_.getAllOperations();
  const auto &deferred = process_model_.getDeferredLoweringStats();

  auto countRequestStates = [&](RequestCompletionState state) {
    auto requestStatePriority = [](RequestCompletionState value) {
      switch (value) {
      case RequestCompletionState::Pending:
        return 0;
      case RequestCompletionState::MayComplete:
        return 1;
      case RequestCompletionState::MustComplete:
        return 2;
      case RequestCompletionState::Terminal:
        return 3;
      }
      return 0;
    };

    std::unordered_map<RequestID, RequestCompletionState> request_states;
    for (const auto &op : operations) {
      if (op.kind != MPIOpKind::SEND_NONBLOCKING &&
          op.kind != MPIOpKind::RECV_NONBLOCKING &&
          op.kind != MPIOpKind::BARRIER_NONBLOCKING &&
          op.kind != MPIOpKind::COLLECTIVE_NONBLOCKING) {
        continue;
      }
      if (!op.request) {
        continue;
      }
      auto it = request_states.find(op.request);
      if (it == request_states.end() || requestStatePriority(op.request_state) >
                                            requestStatePriority(it->second)) {
        request_states[op.request] = op.request_state;
      }
    }

    size_t count = 0;
    for (const auto &entry : request_states) {
      if (entry.second == state) {
        ++count;
      }
    }
    return count;
  };

  size_t deferred_total = 0;
  for (const auto &entry : deferred) {
    deferred_total += entry.second;
  }

  OS << "========================================\n";
  OS << "MPI Analysis Results\n";
  OS << "========================================\n\n";

  OS << "Total MPI operations found: " << operations.size() << "\n";
  OS << "MPI init/finalize ops: " << getOperationCount(MPIOpKind::INIT) << "/"
     << getOperationCount(MPIOpKind::FINALIZE) << "\n";
  OS << "MPI session ops: " << getOperationCount(MPIOpKind::SESSION) << "\n";
  OS << "Blocking point-to-point ops: "
     << getOperationCount(MPIOpKind::SEND_BLOCKING) +
            getOperationCount(MPIOpKind::RECV_BLOCKING)
     << "\n";
  OS << "Non-blocking MPI operations: "
     << getOperationCount(MPIOpKind::SEND_NONBLOCKING) +
            getOperationCount(MPIOpKind::RECV_NONBLOCKING) +
            getOperationCount(MPIOpKind::BARRIER_NONBLOCKING) +
            getOperationCount(MPIOpKind::COLLECTIVE_NONBLOCKING)
     << "\n";
  OS << "  Non-blocking point-to-point ops: "
     << getOperationCount(MPIOpKind::SEND_NONBLOCKING) +
            getOperationCount(MPIOpKind::RECV_NONBLOCKING)
     << "\n";
  OS << "  Probe ops (blocking/non-blocking): "
     << getOperationCount(MPIOpKind::PROBE_BLOCKING) << "/"
     << getOperationCount(MPIOpKind::PROBE_NONBLOCKING) << "\n";
  OS << "  Wait/Test ops: " << getOperationCount(MPIOpKind::WAIT) << "/"
     << getOperationCount(MPIOpKind::TEST) << "\n";
  OS << "Collective/barrier operations: "
     << getOperationCount(MPIOpKind::BARRIER_BLOCKING) +
            getOperationCount(MPIOpKind::BARRIER_NONBLOCKING) +
            getOperationCount(MPIOpKind::COLLECTIVE_BLOCKING) +
            getOperationCount(MPIOpKind::COLLECTIVE_NONBLOCKING)
     << "\n";
  OS << "  Blocking collective/barrier ops: "
     << getOperationCount(MPIOpKind::BARRIER_BLOCKING) +
            getOperationCount(MPIOpKind::COLLECTIVE_BLOCKING)
     << "\n";
  OS << "  Non-blocking collective/barrier ops: "
     << getOperationCount(MPIOpKind::BARRIER_NONBLOCKING) +
            getOperationCount(MPIOpKind::COLLECTIVE_NONBLOCKING)
     << "\n";
  OS << "Communicator management ops: "
     << getOperationCount(MPIOpKind::COMM_MANAGEMENT) << "\n";
  OS << "Intercommunicator creation ops: "
     << getOperationCount(MPIOpKind::INTERCOMM_CREATION) << "\n";
  OS << "Request management ops: "
     << getOperationCount(MPIOpKind::REQUEST_MANAGEMENT) << "\n";
  OS << "RMA window lifecycle ops: " << getOperationCount(MPIOpKind::RMA_WINDOW)
     << "\n";
  OS << "RMA data ops: " << getOperationCount(MPIOpKind::RMA_DATA) << "\n";
  OS << "RMA sync ops: " << getOperationCount(MPIOpKind::RMA_SYNC) << "\n";
  OS << "Collective protocol slots tracked: "
     << getProtocolDiagnosticCount("collective_slots_tracked") << "\n";
  OS << "Collective partial-reachability observations: "
     << getProtocolDiagnosticCount("collective_partial_reachability") << "\n";
  OS << "Requests with may-complete status: "
     << countRequestStates(RequestCompletionState::MayComplete) << "\n";
  OS << "Requests with terminal status: "
     << countRequestStates(RequestCompletionState::Terminal) << "\n";
  OS << "Deferred MPI semantic lowering total: " << deferred_total << "\n\n";

  OS << "Orphaned non-blocking operations: "
     << results_.orphaned_requests.size() << "\n";
  for (const auto &req : results_.orphaned_requests) {
    OS << "  ";
    req.issue_inst->print(OS);
    OS << "\n";
  }
  OS << "\n";

  OS << "Potential deadlocks: " << results_.potential_deadlocks.size() << "\n";
  for (const auto &pair : results_.potential_deadlocks) {
    OS << "  Send: ";
    pair.first->print(OS);
    OS << "\n  Recv: ";
    pair.second->print(OS);
    OS << "\n\n";
  }

  OS << "Mismatched collectives: " << results_.mismatched_collectives.size()
     << "\n";
  for (const auto &pair : results_.mismatched_collectives) {
    OS << "  Collective 1: ";
    pair.first.inst->print(OS);
    OS << "\n  Collective 2: ";
    pair.second.inst->print(OS);
    OS << "\n\n";
  }

  OS << "Conditional collectives (may not be called by all processes): "
     << results_.conditional_collectives.size() << "\n";
  for (const auto *inst : results_.conditional_collectives) {
    OS << "  ";
    inst->print(OS);
    OS << "\n";
  }
  OS << "\n";

  OS << "Unsynchronized RMA operations: " << results_.unsynchronized_rma.size()
     << "\n";
  for (const auto &op : results_.unsynchronized_rma) {
    OS << "  ";
    op.inst->print(OS);
    OS << "\n";
  }
  OS << "\n";

  OS << "Potential RMA data races: " << results_.rma_races.size() << "\n";
  for (const auto &pair : results_.rma_races) {
    OS << "  Op 1: ";
    pair.first.inst->print(OS);
    OS << "\n  Op 2: ";
    pair.second.inst->print(OS);
    OS << "\n\n";
  }

  OS << "Tracked RMA windows: " << getTrackedWindowCount() << "\n";
  OS << "Leaked RMA windows: " << results_.leaked_windows.size() << "\n\n";

  if (!deferred.empty()) {
    OS << "Deferred MPI semantic lowering:\n";
    for (const auto &entry : deferred) {
      OS << "  " << entry.first << ": " << entry.second << "\n";
    }
    OS << "\n";
  }

  OS << "========================================\n";
}

} // namespace mpi
