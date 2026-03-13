#include "Checker/Concurrency/OpenMPChecker.h"

#include <llvm/IR/InstIterator.h>

using namespace llvm;

namespace concurrency {

OpenMPChecker::OpenMPChecker(Module &module, OpenMP::OpenMPTaskGraph *task_graph,
                             ThreadAPI *thread_api)
    : m_module(module), m_taskGraph(task_graph), m_threadAPI(thread_api) {}

void OpenMPChecker::ensureTaskGraph() {
  if (m_taskGraph) {
    return;
  }
  m_ownedTaskGraph = std::make_unique<OpenMP::OpenMPTaskGraph>(m_module);
  m_ownedTaskGraph->analyze();
  m_taskGraph = m_ownedTaskGraph.get();
}

std::vector<ConcurrencyBugReport> OpenMPChecker::checkPartialTaskSynchronization() const {
  std::vector<ConcurrencyBugReport> reports;
  if (!m_taskGraph) {
    return reports;
  }

  for (const auto &boundary : m_taskGraph->getWaitBoundaries()) {
    if (!boundary.is_partial_wait) {
      continue;
    }

    ConcurrencyBugReport report(
        ConcurrencyBugType::OPENMP_PARTIAL_SYNC,
        "OpenMP task wait with dependencies may leave sibling tasks unsynchronized",
        BugDescription::BI_MEDIUM, BugDescription::BC_ERROR);
    report.addStep(boundary.inst,
                   "Selective __kmpc_omp_wait_deps boundary encountered here");
    reports.push_back(std::move(report));
  }

  return reports;
}

std::vector<ConcurrencyBugReport> OpenMPChecker::checkTaskgroupStructure() const {
  std::vector<ConcurrencyBugReport> reports;
  if (!m_threadAPI) {
    return reports;
  }

  for (const Function &func : m_module) {
    if (func.isDeclaration()) {
      continue;
    }

    int depth = 0;
    const Instruction *last_start = nullptr;
    for (const Instruction &inst : instructions(func)) {
      const auto *call = dyn_cast<CallBase>(&inst);
      if (!call) {
        continue;
      }
      auto type = m_threadAPI->getType(call);
      if (type == ThreadAPI::TD_OMP_TASKGROUP_START) {
        ++depth;
        last_start = &inst;
      } else if (type == ThreadAPI::TD_OMP_TASKGROUP_END) {
        if (depth == 0) {
          ConcurrencyBugReport report(
              ConcurrencyBugType::OPENMP_TASKGROUP_MISMATCH,
              "OpenMP taskgroup end has no matching start",
              BugDescription::BI_HIGH, BugDescription::BC_ERROR);
          report.addStep(&inst, "Encountered __kmpc_end_taskgroup without active taskgroup");
          reports.push_back(std::move(report));
        } else {
          --depth;
        }
      }
    }

    if (depth > 0 && last_start) {
      ConcurrencyBugReport report(
          ConcurrencyBugType::OPENMP_TASKGROUP_MISMATCH,
          "OpenMP taskgroup start may not be properly closed",
          BugDescription::BI_HIGH, BugDescription::BC_ERROR);
      report.addStep(last_start,
                     "Taskgroup starts here but no matching __kmpc_end_taskgroup was found");
      reports.push_back(std::move(report));
    }
  }

  return reports;
}

std::vector<ConcurrencyBugReport> OpenMPChecker::checkAtomicRegionStructure() const {
  std::vector<ConcurrencyBugReport> reports;
  if (!m_threadAPI) {
    return reports;
  }

  for (const Function &func : m_module) {
    if (func.isDeclaration()) {
      continue;
    }

    int depth = 0;
    const Instruction *last_start = nullptr;
    for (const Instruction &inst : instructions(func)) {
      const auto *call = dyn_cast<CallBase>(&inst);
      if (!call) {
        continue;
      }
      auto type = m_threadAPI->getType(call);
      if (type == ThreadAPI::TD_OMP_ATOMIC_START) {
        ++depth;
        last_start = &inst;
      } else if (type == ThreadAPI::TD_OMP_ATOMIC_END) {
        if (depth == 0) {
          ConcurrencyBugReport report(
              ConcurrencyBugType::OPENMP_ATOMIC_MISMATCH,
              "OpenMP atomic end has no matching start",
              BugDescription::BI_HIGH, BugDescription::BC_ERROR);
          report.addStep(&inst, "Encountered __kmpc_atomic_end without active atomic region");
          reports.push_back(std::move(report));
        } else {
          --depth;
        }
      }
    }

    if (depth > 0 && last_start) {
      ConcurrencyBugReport report(
          ConcurrencyBugType::OPENMP_ATOMIC_MISMATCH,
          "OpenMP atomic region may not be properly closed",
          BugDescription::BI_HIGH, BugDescription::BC_ERROR);
      report.addStep(last_start,
                     "Atomic region starts here but no matching __kmpc_atomic_end was found");
      reports.push_back(std::move(report));
    }
  }

  return reports;
}

std::vector<ConcurrencyBugReport> OpenMPChecker::checkOpenMPBugs() {
  ensureTaskGraph();

  std::vector<ConcurrencyBugReport> reports;

  auto append = [&](std::vector<ConcurrencyBugReport> more) {
    reports.insert(reports.end(), std::make_move_iterator(more.begin()),
                   std::make_move_iterator(more.end()));
  };

  append(checkPartialTaskSynchronization());
  append(checkTaskgroupStructure());
  append(checkAtomicRegionStructure());

  return reports;
}

} // namespace concurrency