#include "Analysis/Concurrency/ConcurrencyFacade.h"

namespace concurrency {

ConcurrencyFacade::OpenMPSummary
ConcurrencyFacade::analyzeOpenMP(llvm::Module &module) {
  OpenMP::OpenMPTaskGraph graph(module);
  graph.analyze();

  OpenMPSummary summary;
  summary.task_count = graph.getAllTasks().size();
  summary.wait_boundary_count = graph.getWaitBoundaries().size();
  summary.unknown_relation_count = graph.getUnknownReasonCounts().size();
  summary.deferred_wait_dep_count = graph.getDeferredWaitDepsCount();
  summary.deferred_conflict_count = graph.getDeferredImpreciseConflictCount();
  return summary;
}

ConcurrencyFacade::MPISummary
ConcurrencyFacade::analyzeMPI(llvm::Module &module) {
  mpi::MPIAnalysis analysis(module);
  analysis.runAnalysis();

  const auto &results = analysis.getResults();
  MPISummary summary;
  summary.operation_count = analysis.getProcessModel().getAllOperations().size();
  summary.orphaned_request_count = results.orphaned_requests.size();
  summary.potential_deadlock_count = results.potential_deadlocks.size();
  summary.mismatched_collective_count = results.mismatched_collectives.size();
  summary.conditional_collective_count = results.conditional_collectives.size();
  summary.rma_race_count = results.rma_races.size();
  return summary;
}

} // namespace concurrency
