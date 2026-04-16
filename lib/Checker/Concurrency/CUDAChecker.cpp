#include "Checker/Concurrency/CUDAChecker.h"

using namespace llvm;

namespace concurrency {

CUDAChecker::CUDAChecker(Module &module, cuda::CUDAAnalysis *analysis)
    : m_module(module), m_analysis(analysis) {
  if (!m_analysis) {
    m_owned_analysis = std::make_unique<cuda::CUDAAnalysis>(m_module);
    m_owned_analysis->runAnalysis();
    m_analysis = m_owned_analysis.get();
  }
}

std::vector<ConcurrencyBugReport> CUDAChecker::checkCUDABugs() {
  std::vector<ConcurrencyBugReport> reports;
  if (!m_analysis) {
    return reports;
  }

  for (const cuda::KernelSummary &summary : m_analysis->getKernelSummaries()) {
    if (summary.has_warp_divergence) {
      ConcurrencyBugReport report(
          ConcurrencyBugType::CUDA_WARP_DIVERGENCE,
          "CUDA warp divergence from thread-dependent control flow",
          BugDescription::BI_MEDIUM, BugDescription::BC_PERFORMANCE);
      for (const auto &region : summary.divergence_regions) {
        report.addStep(region.branch,
                       region.depends_on_lane_id
                           ? "Lane-dependent branch splits a warp execution path"
                           : "Thread-index-dependent branch splits a warp execution path");
      }
      reports.push_back(std::move(report));
    }

    if (summary.has_shared_race) {
      ConcurrencyBugReport report(
          ConcurrencyBugType::CUDA_SHARED_MEMORY_RACE,
          "Potential shared-memory race inside a block",
          BugDescription::BI_HIGH, BugDescription::BC_ERROR);
      for (const auto &race : summary.shared_races) {
        report.addStep(race.first,
                       "Conflicting shared-memory access participates in the race");
        report.addStep(race.second,
                       "Another block-local thread may access the same shared location");
      }
      reports.push_back(std::move(report));
    }

    if (summary.has_global_race) {
      ConcurrencyBugReport report(
          ConcurrencyBugType::CUDA_GLOBAL_MEMORY_RACE,
          "Potential global/device-memory race across CUDA threads",
          BugDescription::BI_HIGH, BugDescription::BC_ERROR);
      for (const auto &race : summary.global_races) {
        report.addStep(
            race.first,
            race.cross_block
                ? "Conflicting global access may be reachable from different blocks"
                : "Conflicting global access may be reachable from different threads");
        report.addStep(race.second,
                       "Second conflicting access aliases the same global/device object");
      }
      reports.push_back(std::move(report));
    }

    if (summary.has_barrier_mismatch) {
      ConcurrencyBugReport report(
          ConcurrencyBugType::CUDA_BARRIER_MISMATCH,
          "Potential CUDA barrier mismatch or deadlock",
          BugDescription::BI_HIGH, BugDescription::BC_ERROR);
      for (const auto &mismatch : summary.barrier_mismatches) {
        report.addStep(mismatch.branch,
                       "Divergent branch controls whether threads reach a barrier");
        report.addStep(mismatch.barrier,
                       "Barrier is nested in only part of the divergent region");
      }
      reports.push_back(std::move(report));
    }

    if (summary.has_bank_conflict) {
      ConcurrencyBugReport report(
          ConcurrencyBugType::CUDA_BANK_CONFLICT,
          "Shared-memory bank conflict detected from per-lane bank mapping",
          BugDescription::BI_MEDIUM, BugDescription::BC_PERFORMANCE);
      for (const auto &conflict : summary.bank_conflicts) {
        std::string description =
            "Shared-memory access maps multiple lanes to the same bank with "
            "estimated conflict degree " +
            std::to_string(conflict.conflict_degree);
        report.addStep(
            conflict.inst, description);
      }
      reports.push_back(std::move(report));
    }

    if (summary.has_uncoalesced_access) {
      ConcurrencyBugReport report(
          ConcurrencyBugType::CUDA_UNCOALESCED_ACCESS,
          "Global-memory access is not fully coalesced",
          BugDescription::BI_MEDIUM, BugDescription::BC_PERFORMANCE);
      for (const auto &issue : summary.coalescing_issues) {
        std::string description =
            "Warp access is " +
            std::string(cuda::CUDAAnalysis::toString(issue.quality)) +
            " with about " + std::to_string(issue.estimated_transactions) +
            " memory transactions";
        report.addStep(
            issue.inst, description);
      }
      reports.push_back(std::move(report));
    }

    if (summary.has_volatile_missing) {
      ConcurrencyBugReport report(
          ConcurrencyBugType::CUDA_VOLATILE_MISSING,
          "Potential missing volatile qualifier on inter-thread CUDA memory",
          BugDescription::BI_LOW, BugDescription::BC_WARNING);
      for (const auto &missing : summary.volatile_missing) {
        report.addStep(missing.inst,
                       "Non-atomic inter-thread memory is accessed without volatile");
      }
      reports.push_back(std::move(report));
    }

    if (summary.dimensions.hasSymbolicGrid() || summary.dimensions.hasSymbolicBlock()) {
      ConcurrencyBugReport report(
          ConcurrencyBugType::CUDA_SYMBOLIC_CONFIG_RISK,
          "CUDA launch configuration is symbolic or parameterized",
          BugDescription::BI_LOW, BugDescription::BC_WARNING);
      if (summary.kernel && !summary.kernel->empty() &&
          !summary.kernel->front().empty()) {
        report.addStep(&summary.kernel->front().front(),
                       "Kernel is analyzed under symbolic grid/block dimensions");
      }
      reports.push_back(std::move(report));
    }

    for (const cuda::AccessInfo &access : summary.accesses) {
      if (access.space == cuda::MemorySpace::Unknown && access.inst) {
        ConcurrencyBugReport report(
            ConcurrencyBugType::CUDA_SHARED_GLOBAL_SPACE_MISMATCH,
            "CUDA memory space could not be classified precisely",
            BugDescription::BI_LOW, BugDescription::BC_WARNING);
        report.addStep(access.inst,
                       "Address space was not resolved to shared/global/local/constant");
        reports.push_back(std::move(report));
        break;
      }
    }
  }

  return reports;
}

} // namespace concurrency
