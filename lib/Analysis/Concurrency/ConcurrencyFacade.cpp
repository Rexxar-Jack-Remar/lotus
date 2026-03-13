#include "Analysis/Concurrency/ConcurrencyFacade.h"

#include "Analysis/Concurrency/MPI/MPIAnalysis.h"
#include "Analysis/Concurrency/OpenMP/OpenMPTaskGraph.h"

#include <numeric>

namespace concurrency {

ConcurrencyFacade::OpenMPSummary
ConcurrencyFacade::analyzeOpenMP(llvm::Module &module) {
  OpenMP::OpenMPTaskGraph graph(module);
  graph.analyze();

  const auto &graph_summary = graph.getSummary();
  OpenMPSummary summary;
  summary.task_count = graph_summary.task_count;
  summary.task_with_dependencies_count =
      graph_summary.task_with_dependencies_count;
  summary.included_task_count = graph_summary.included_task_count;
  summary.final_task_count = graph_summary.final_task_count;
  summary.untied_task_count = graph_summary.untied_task_count;
  summary.detached_task_count = graph_summary.detached_task_count;
  summary.taskloop_count = graph_summary.taskloop_count;
  summary.wait_boundary_count = graph_summary.wait_boundary_count;
  summary.partial_wait_boundary_count =
      graph_summary.partial_wait_boundary_count;
  summary.taskgroup_region_count = graph_summary.taskgroup_region_count;
  summary.single_region_count = graph_summary.single_region_count;
  summary.master_region_count = graph_summary.master_region_count;
  summary.ordered_region_count = graph_summary.ordered_region_count;
  summary.sections_region_count = graph_summary.sections_region_count;
  summary.worksharing_loop_count = graph_summary.worksharing_loop_count;
  summary.reduction_region_count = graph_summary.reduction_region_count;
  summary.worksharing_region_count =
      graph_summary.single_region_count + graph_summary.sections_region_count +
      graph_summary.worksharing_loop_count +
      graph_summary.reduction_region_count +
      graph_summary.ordered_region_count + graph_summary.master_region_count;
  summary.atomic_region_count = graph_summary.atomic_region_count;
  summary.flush_count = graph_summary.flush_count;
  summary.cancel_count = graph_summary.cancel_count;
  summary.target_region_count = graph_summary.target_region_count;
  summary.target_data_region_count = graph_summary.target_data_region_count;
  summary.detach_completion_count = graph_summary.detach_completion_count;
  summary.happens_before_relation_count =
      graph.getRelationCount(concurrency::RelationKind::MustHappenBefore);
  summary.exclusion_relation_count =
      graph.getRelationCount(concurrency::RelationKind::MutuallyExclusive);
  summary.unknown_relation_count =
      graph.getRelationCount(concurrency::RelationKind::UnknownDueToModelGap);
  summary.unknown_reason_bucket_count = graph.getUnknownReasonCounts().size();
  summary.deferred_wait_dep_count = graph.getDeferredWaitDepsCount();
  summary.deferred_conflict_count = graph.getDeferredImpreciseConflictCount();
  return summary;
}

ConcurrencyFacade::MPISummary
ConcurrencyFacade::analyzeMPI(llvm::Module &module) {
  mpi::MPIAnalysis analysis(module);
  analysis.runAnalysis();

  const auto &results = analysis.getResults();
  const auto &operations = analysis.getProcessModel().getAllOperations();
  const auto &deferred = analysis.getProcessModel().getDeferredLoweringStats();
  MPISummary summary;
  summary.operation_count = operations.size();
  summary.init_count = analysis.getOperationCount(mpi::MPIOpKind::INIT);
  summary.finalize_count = analysis.getOperationCount(mpi::MPIOpKind::FINALIZE);
  summary.blocking_point_to_point_count =
      analysis.getOperationCount(mpi::MPIOpKind::SEND_BLOCKING) +
      analysis.getOperationCount(mpi::MPIOpKind::RECV_BLOCKING);
  summary.nonblocking_operation_count =
      analysis.getOperationCount(mpi::MPIOpKind::SEND_NONBLOCKING) +
      analysis.getOperationCount(mpi::MPIOpKind::RECV_NONBLOCKING) +
      analysis.getOperationCount(mpi::MPIOpKind::BARRIER_NONBLOCKING) +
      analysis.getOperationCount(mpi::MPIOpKind::COLLECTIVE_NONBLOCKING);
  summary.nonblocking_point_to_point_count =
      analysis.getOperationCount(mpi::MPIOpKind::SEND_NONBLOCKING) +
      analysis.getOperationCount(mpi::MPIOpKind::RECV_NONBLOCKING);
  summary.probe_operation_count =
      analysis.getOperationCount(mpi::MPIOpKind::PROBE_BLOCKING) +
      analysis.getOperationCount(mpi::MPIOpKind::PROBE_NONBLOCKING);
  summary.wait_operation_count =
      analysis.getOperationCount(mpi::MPIOpKind::WAIT);
  summary.test_operation_count =
      analysis.getOperationCount(mpi::MPIOpKind::TEST);
  summary.collective_operation_count =
      analysis.getOperationCount(mpi::MPIOpKind::BARRIER_BLOCKING) +
      analysis.getOperationCount(mpi::MPIOpKind::BARRIER_NONBLOCKING) +
      analysis.getOperationCount(mpi::MPIOpKind::COLLECTIVE_BLOCKING) +
      analysis.getOperationCount(mpi::MPIOpKind::COLLECTIVE_NONBLOCKING);
  summary.blocking_collective_count =
      analysis.getOperationCount(mpi::MPIOpKind::BARRIER_BLOCKING) +
      analysis.getOperationCount(mpi::MPIOpKind::COLLECTIVE_BLOCKING);
  summary.nonblocking_collective_count =
      analysis.getOperationCount(mpi::MPIOpKind::BARRIER_NONBLOCKING) +
      analysis.getOperationCount(mpi::MPIOpKind::COLLECTIVE_NONBLOCKING);
  summary.communicator_management_count =
      analysis.getOperationCount(mpi::MPIOpKind::COMM_MANAGEMENT);
  summary.request_management_count =
      analysis.getOperationCount(mpi::MPIOpKind::REQUEST_MANAGEMENT);
  summary.rma_window_count =
      analysis.getOperationCount(mpi::MPIOpKind::RMA_WINDOW);
  summary.rma_operation_count =
      analysis.getOperationCount(mpi::MPIOpKind::RMA_DATA);
  summary.rma_sync_count = analysis.getOperationCount(mpi::MPIOpKind::RMA_SYNC);
  auto requestStatePriority = [](mpi::RequestCompletionState state) {
    switch (state) {
    case mpi::RequestCompletionState::Pending:
      return 0;
    case mpi::RequestCompletionState::MayComplete:
      return 1;
    case mpi::RequestCompletionState::MustComplete:
      return 2;
    case mpi::RequestCompletionState::Terminal:
      return 3;
    }
    return 0;
  };
  std::unordered_map<mpi::RequestID, mpi::RequestCompletionState>
      request_states;
  for (const auto &op : operations) {
    if (op.kind != mpi::MPIOpKind::SEND_NONBLOCKING &&
        op.kind != mpi::MPIOpKind::RECV_NONBLOCKING &&
        op.kind != mpi::MPIOpKind::BARRIER_NONBLOCKING &&
        op.kind != mpi::MPIOpKind::COLLECTIVE_NONBLOCKING) {
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
  for (const auto &entry : request_states) {
    if (entry.second == mpi::RequestCompletionState::MayComplete) {
      ++summary.may_complete_request_count;
    }
    if (entry.second == mpi::RequestCompletionState::Terminal) {
      ++summary.terminal_request_count;
    }
  }
  summary.orphaned_request_count = results.orphaned_requests.size();
  summary.potential_deadlock_count = results.potential_deadlocks.size();
  summary.mismatched_collective_count = results.mismatched_collectives.size();
  summary.conditional_collective_count = results.conditional_collectives.size();
  summary.collective_partial_reachability_count =
      analysis.getProtocolDiagnosticCount("collective_partial_reachability");
  summary.unsynchronized_rma_count = results.unsynchronized_rma.size();
  summary.rma_race_count = results.rma_races.size();
  summary.tracked_window_count = analysis.getTrackedWindowCount();
  summary.leaked_window_count = results.leaked_windows.size();
  summary.collective_slot_count =
      analysis.getProtocolDiagnosticCount("collective_slots_tracked");
  summary.deferred_semantic_lowering_count = std::accumulate(
      deferred.begin(), deferred.end(), size_t{0},
      [](size_t total, const std::pair<const std::string, size_t> &entry) {
        return total + entry.second;
      });
  return summary;
}

void ConcurrencyFacade::printOpenMPResults(llvm::Module &module,
                                           llvm::raw_ostream &os) {
  const OpenMPSummary summary = analyzeOpenMP(module);
  os << "========================================\n";
  os << "OpenMP Analysis Results\n";
  os << "========================================\n\n";
  os << "Tasks: " << summary.task_count << "\n";
  os << "  Task-with-deps: " << summary.task_with_dependencies_count << "\n";
  os << "  Included/final/untied/detached: " << summary.included_task_count
     << "/" << summary.final_task_count << "/" << summary.untied_task_count
     << "/" << summary.detached_task_count << "\n";
  os << "  Taskloop tasks: " << summary.taskloop_count << "\n";
  os << "Scheduling boundaries (wait/partial/taskgroup): "
     << summary.wait_boundary_count << "/"
     << summary.partial_wait_boundary_count << "/"
     << summary.taskgroup_region_count << "\n";
  os << "Worksharing regions "
        "(total/single/master/ordered/sections/loops/reduction): "
     << summary.worksharing_region_count << "/" << summary.single_region_count
     << "/" << summary.master_region_count << "/"
     << summary.ordered_region_count << "/" << summary.sections_region_count
     << "/" << summary.worksharing_loop_count << "/"
     << summary.reduction_region_count << "\n";
  os << "Atomic/flush/cancel regions: " << summary.atomic_region_count << "/"
     << summary.flush_count << "/" << summary.cancel_count << "\n";
  os << "Target regions (target/target-data): " << summary.target_region_count
     << "/" << summary.target_data_region_count << "\n";
  os << "Detach completions: " << summary.detach_completion_count << "\n";
  os << "Task relations (HB/exclusion/unknown): "
     << summary.happens_before_relation_count << "/"
     << summary.exclusion_relation_count << "/"
     << summary.unknown_relation_count << "\n";
  os << "Unknown relation reason buckets: "
     << summary.unknown_reason_bucket_count << "\n";
  os << "Deferred modeling (wait-deps/conflicts): "
     << summary.deferred_wait_dep_count << "/"
     << summary.deferred_conflict_count << "\n";
  os << "========================================\n";
}

} // namespace concurrency
