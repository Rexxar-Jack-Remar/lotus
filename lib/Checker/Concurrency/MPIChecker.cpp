#include "Checker/Concurrency/MPIChecker.h"

using namespace llvm;

namespace concurrency {

MPIChecker::MPIChecker(Module &module, mpi::MPIAnalysis *analysis)
    : m_module(module), m_analysis(analysis) {}

void MPIChecker::ensureAnalysis() {
  if (m_analysis) {
    return;
  }
  m_ownedAnalysis = std::make_unique<mpi::MPIAnalysis>(m_module);
  m_ownedAnalysis->runAnalysis();
  m_analysis = m_ownedAnalysis.get();
}

std::vector<ConcurrencyBugReport> MPIChecker::checkMPIBugs() {
  ensureAnalysis();

  std::vector<ConcurrencyBugReport> reports;
  if (!m_analysis) {
    return reports;
  }

  const auto &results = m_analysis->getResults();

  for (const auto &req : results.orphaned_requests) {
    ConcurrencyBugReport report(
        ConcurrencyBugType::MPI_ORPHANED_REQUEST,
        "MPI non-blocking request may never be completed",
        BugDescription::BI_HIGH, BugDescription::BC_ERROR);
    report.addStep(req.issue_inst,
                   "Non-blocking MPI request is issued here without a matching completion");
    reports.push_back(std::move(report));
  }

  for (const auto &pair : results.potential_deadlocks) {
    ConcurrencyBugReport report(
        ConcurrencyBugType::MPI_DEADLOCK,
        "Potential MPI blocking communication deadlock",
        BugDescription::BI_HIGH, BugDescription::BC_ERROR);
    report.addStep(pair.first,
                   "Blocking communication participating in a potential deadlock cycle");
    report.addStep(pair.second,
                   "Another blocking communication may wait cyclically with the first");
    reports.push_back(std::move(report));
  }

  for (const auto &pair : results.mismatched_collectives) {
    ConcurrencyBugReport report(
        ConcurrencyBugType::MPI_COLLECTIVE_MISMATCH,
        "Mismatched MPI collective operations across processes",
        BugDescription::BI_HIGH, BugDescription::BC_ERROR);
    report.addStep(pair.first.inst,
                   "Collective call is incompatible with a peer collective");
    report.addStep(pair.second.inst,
                   "Conflicting collective call observed here");
    reports.push_back(std::move(report));
  }

  for (const Instruction *inst : results.conditional_collectives) {
    ConcurrencyBugReport report(
        ConcurrencyBugType::MPI_CONDITIONAL_COLLECTIVE,
        "MPI collective may be executed conditionally by only a subset of ranks",
        BugDescription::BI_HIGH, BugDescription::BC_ERROR);
    report.addStep(inst,
                   "Collective call appears rank-guarded or control-dependent on rank");
    reports.push_back(std::move(report));
  }

  for (const auto &op : results.unsynchronized_rma) {
    ConcurrencyBugReport report(
        ConcurrencyBugType::MPI_UNSYNC_RMA,
        "MPI RMA operation may execute without a proven synchronization epoch",
        BugDescription::BI_HIGH, BugDescription::BC_ERROR);
    report.addStep(op.inst,
                   "RMA access occurs here without recognized synchronization");
    reports.push_back(std::move(report));
  }

  for (const auto &pair : results.rma_races) {
    ConcurrencyBugReport report(
        ConcurrencyBugType::MPI_RMA_RACE,
        "Potential MPI RMA data race",
        BugDescription::BI_HIGH, BugDescription::BC_ERROR);
    report.addStep(pair.first.inst,
                   "First conflicting RMA operation occurs here");
    report.addStep(pair.second.inst,
                   "Second conflicting RMA operation occurs here");
    reports.push_back(std::move(report));
  }

  for (auto window : results.leaked_windows) {
    (void)window;
    ConcurrencyBugReport report(
        ConcurrencyBugType::MPI_WINDOW_LEAK,
        "MPI RMA window may be leaked",
        BugDescription::BI_MEDIUM, BugDescription::BC_ERROR);
    reports.push_back(std::move(report));
  }

  return reports;
}

} // namespace concurrency