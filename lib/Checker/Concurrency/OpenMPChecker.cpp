#include "Checker/Concurrency/OpenMPChecker.h"

#include <llvm/IR/InstIterator.h>

using namespace llvm;

namespace concurrency {

OpenMPChecker::OpenMPChecker(Module &module,
                             OpenMP::OpenMPTaskGraph *task_graph,
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

std::vector<ConcurrencyBugReport>
OpenMPChecker::checkPartialTaskSynchronization() const {
  std::vector<ConcurrencyBugReport> reports;
  if (!m_taskGraph) {
    return reports;
  }

  for (const auto &boundary : m_taskGraph->getWaitBoundaries()) {
    if (!boundary.is_partial_wait) {
      continue;
    }

    ConcurrencyBugReport report(ConcurrencyBugType::OPENMP_PARTIAL_SYNC,
                                "OpenMP task wait with dependencies may leave "
                                "sibling tasks unsynchronized",
                                BugDescription::BI_MEDIUM,
                                BugDescription::BC_ERROR);
    report.addStep(boundary.inst,
                   "Selective __kmpc_omp_wait_deps boundary encountered here");
    reports.push_back(std::move(report));
  }

  return reports;
}

std::vector<ConcurrencyBugReport>
OpenMPChecker::checkTaskgroupStructure() const {
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
          report.addStep(
              &inst,
              "Encountered __kmpc_end_taskgroup without active taskgroup");
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
      report.addStep(last_start, "Taskgroup starts here but no matching "
                                 "__kmpc_end_taskgroup was found");
      reports.push_back(std::move(report));
    }
  }

  return reports;
}

std::vector<ConcurrencyBugReport>
OpenMPChecker::checkAtomicRegionStructure() const {
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
          report.addStep(
              &inst,
              "Encountered __kmpc_atomic_end without active atomic region");
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
      report.addStep(last_start, "Atomic region starts here but no matching "
                                 "__kmpc_atomic_end was found");
      reports.push_back(std::move(report));
    }
  }

  return reports;
}

std::vector<ConcurrencyBugReport> OpenMPChecker::checkDetachedTaskLeak() const {
  std::vector<ConcurrencyBugReport> reports;
  if (!m_taskGraph) {
    return reports;
  }

  const auto &summary = m_taskGraph->getSummary();
  if (summary.detached_task_count > summary.detach_completion_count) {
    ConcurrencyBugReport report(
        ConcurrencyBugType::OPENMP_DETACHED_TASK_LEAK,
        "OpenMP detached task may not be properly completed",
        BugDescription::BI_MEDIUM, BugDescription::BC_ERROR);
    report.addStep(
        nullptr,
        "Detached tasks: " + std::to_string(summary.detached_task_count) +
            ", completions: " +
            std::to_string(summary.detach_completion_count));
    reports.push_back(std::move(report));
  }

  return reports;
}

std::vector<ConcurrencyBugReport> OpenMPChecker::checkNestedSingle() const {
  std::vector<ConcurrencyBugReport> reports;
  if (!m_threadAPI) {
    return reports;
  }

  for (const Function &func : m_module) {
    if (func.isDeclaration()) {
      continue;
    }

    int single_depth = 0;
    const Instruction *outer_single = nullptr;
    for (const Instruction &inst : instructions(func)) {
      const auto *call = dyn_cast<CallBase>(&inst);
      if (!call) {
        continue;
      }
      auto type = m_threadAPI->getType(call);
      if (type == ThreadAPI::TD_OMP_SINGLE_START) {
        if (single_depth > 0) {
          ConcurrencyBugReport report(ConcurrencyBugType::OPENMP_NESTED_SINGLE,
                                      "Nested OpenMP single regions detected "
                                      "without proper synchronization",
                                      BugDescription::BI_MEDIUM,
                                      BugDescription::BC_ERROR);
          report.addStep(outer_single, "Outer single region");
          report.addStep(
              &inst, "Inner single region - may cause implicit barrier issues");
          reports.push_back(std::move(report));
        }
        ++single_depth;
        if (!outer_single) {
          outer_single = &inst;
        }
      } else if (type == ThreadAPI::TD_OMP_SINGLE_END) {
        if (single_depth > 0) {
          --single_depth;
          if (single_depth == 0) {
            outer_single = nullptr;
          }
        }
      }
    }
  }

  return reports;
}

std::vector<ConcurrencyBugReport>
OpenMPChecker::checkNowaitMissingBarrier() const {
  std::vector<ConcurrencyBugReport> reports;
  if (!m_threadAPI) {
    return reports;
  }

  for (const Function &func : m_module) {
    if (func.isDeclaration()) {
      continue;
    }

    const Instruction *last_workshare = nullptr;
    bool found_flush_after_workshare = false;

    for (const Instruction &inst : instructions(func)) {
      const auto *call = dyn_cast<CallBase>(&inst);
      if (!call) {
        continue;
      }

      const Function *callee = call->getCalledFunction();
      if (!callee) {
        continue;
      }
      StringRef name = callee->getName();

      if (name.contains("single_start") || name.contains("for_static_fini") ||
          name.contains("sections_next")) {
        last_workshare = &inst;
        found_flush_after_workshare = false;
        continue;
      }

      if (name.contains("flush")) {
        if (last_workshare) {
          found_flush_after_workshare = true;
        }
        continue;
      }

      if (name.contains("barrier")) {
        if (found_flush_after_workshare && last_workshare) {
          ConcurrencyBugReport report(
              ConcurrencyBugType::OPENMP_NOWAIT_MISSING_BARRIER,
              "Potential race between workshare region and barrier",
              BugDescription::BI_MEDIUM, BugDescription::BC_ERROR);
          report.addStep(last_workshare, "Workshare region found");
          report.addStep(&inst, "Subsequent flush/barrier found");
          reports.push_back(std::move(report));
        }
        last_workshare = nullptr;
        found_flush_after_workshare = false;
      }
    }
  }

  return reports;
}

std::vector<ConcurrencyBugReport> OpenMPChecker::checkMissingFlush() const {
  std::vector<ConcurrencyBugReport> reports;
  if (!m_threadAPI) {
    return reports;
  }

  for (const Function &func : m_module) {
    if (func.isDeclaration()) {
      continue;
    }

    bool has_critical_section = false;
    bool has_flush = false;

    for (const Instruction &inst : instructions(func)) {
      const auto *call = dyn_cast<CallBase>(&inst);
      if (!call) {
        continue;
      }

      const Function *callee = call->getCalledFunction();
      if (!callee) {
        continue;
      }
      StringRef name = callee->getName();

      if (name.contains("__kmpc_critical") ||
          name.contains("__kmpc_end_critical") ||
          name.contains("omp_set_lock") || name.contains("omp_unset_lock")) {
        has_critical_section = true;
      }

      if (name.contains("__kmpc_flush") ||
          name.contains("omp_get_thread_num")) {
        has_flush = true;
      }
    }

    if (has_critical_section && !has_flush) {
      ConcurrencyBugReport report(
          ConcurrencyBugType::OPENMP_MISSING_FLUSH,
          "OpenMP critical section without flush may cause stale data",
          BugDescription::BI_MEDIUM, BugDescription::BC_ERROR);
      report.addStep(nullptr, "Critical section or lock used without flush");
      reports.push_back(std::move(report));
    }
  }

  return reports;
}

std::vector<ConcurrencyBugReport>
OpenMPChecker::checkIncorrectNumThreads() const {
  std::vector<ConcurrencyBugReport> reports;
  if (!m_threadAPI) {
    return reports;
  }

  for (const Function &func : m_module) {
    if (func.isDeclaration()) {
      continue;
    }

    for (const Instruction &inst : instructions(func)) {
      const auto *call = dyn_cast<CallBase>(&inst);
      if (!call) {
        continue;
      }

      const Function *callee = call->getCalledFunction();
      if (!callee) {
        continue;
      }
      StringRef name = callee->getName();

      if (name.contains("__kmpc_fork_call")) {
        if (call->arg_size() >= 3) {
          const Value *num_threads_arg = call->getArgOperand(2);
          if (const auto *call_inst = dyn_cast<CallInst>(num_threads_arg)) {
            const Function *inner_callee = call_inst->getCalledFunction();
            if (inner_callee &&
                (inner_callee->getName().contains("omp_get_num_threads") ||
                 inner_callee->getName().contains("omp_get_max_threads"))) {
              ConcurrencyBugReport report(
                  ConcurrencyBugType::OPENMP_INCORRECT_NUMTHREADS,
                  "Incorrect use of omp_get_num_threads in num_threads clause",
                  BugDescription::BI_MEDIUM, BugDescription::BC_ERROR);
              report.addStep(
                  &inst, "omp_get_num_threads called in num_threads clause");
              reports.push_back(std::move(report));
            }
          }
        }
      }
    }
  }

  return reports;
}

std::vector<ConcurrencyBugReport> OpenMPChecker::checkReductionError() const {
  std::vector<ConcurrencyBugReport> reports;
  if (!m_threadAPI) {
    return reports;
  }

  for (const Function &func : m_module) {
    if (func.isDeclaration()) {
      continue;
    }

    std::set<const Instruction *> reduce_starts;
    std::set<const Instruction *> reduce_ends;

    for (const Instruction &inst : instructions(func)) {
      const auto *call = dyn_cast<CallBase>(&inst);
      if (!call) {
        continue;
      }

      const Function *callee = call->getCalledFunction();
      if (!callee) {
        continue;
      }
      StringRef name = callee->getName();

      if (name.contains("__kmpc_reduce") && !name.contains("end")) {
        reduce_starts.insert(&inst);
      }
      if (name.contains("__kmpc_end_reduce")) {
        reduce_ends.insert(&inst);
      }
    }

    if (reduce_starts.size() != reduce_ends.size()) {
      for (const auto *start : reduce_starts) {
        ConcurrencyBugReport report(
            ConcurrencyBugType::OPENMP_REDUCTION_ERROR,
            "OpenMP reduction region may be improperly nested",
            BugDescription::BI_MEDIUM, BugDescription::BC_ERROR);
        report.addStep(start, "Reduction start without matching end");
        reports.push_back(std::move(report));
        break;
      }
    }
  }

  return reports;
}

std::vector<ConcurrencyBugReport> OpenMPChecker::checkTaskwaitMissing() const {
  std::vector<ConcurrencyBugReport> reports;
  if (!m_threadAPI) {
    return reports;
  }

  for (const Function &func : m_module) {
    if (func.isDeclaration()) {
      continue;
    }

    bool has_task_create = false;
    const Instruction *task_create_inst = nullptr;

    for (const Instruction &inst : instructions(func)) {
      const auto *call = dyn_cast<CallBase>(&inst);
      if (!call) {
        continue;
      }

      const Function *callee = call->getCalledFunction();
      if (!callee) {
        continue;
      }
      StringRef name = callee->getName();

      if (name.contains("__kmpc_omp_task") && !name.contains("wait")) {
        has_task_create = true;
        task_create_inst = &inst;
      }

      if (name.contains("__kmpc_omp_taskwait") ||
          name.contains("__kmpc_omp_wait_deps")) {
        has_task_create = false;
        task_create_inst = nullptr;
      }

      if (isa<ReturnInst>(&inst) && has_task_create && task_create_inst) {
        ConcurrencyBugReport report(
            ConcurrencyBugType::OMP_TASKWAIT_MISSING,
            "OpenMP task created without taskwait before function returns",
            BugDescription::BI_MEDIUM, BugDescription::BC_ERROR);
        report.addStep(task_create_inst,
                       "Task created here without taskwait before return");
        reports.push_back(std::move(report));
        break;
      }
    }
  }

  return reports;
}

std::vector<ConcurrencyBugReport>
OpenMPChecker::checkNestedParallelDisabled() const {
  std::vector<ConcurrencyBugReport> reports;

  for (const Function &func : m_module) {
    if (func.isDeclaration()) {
      continue;
    }

    for (const Instruction &inst : instructions(func)) {
      const auto *call = dyn_cast<CallBase>(&inst);
      if (!call) {
        continue;
      }

      const Function *callee = call->getCalledFunction();
      if (!callee) {
        continue;
      }
      StringRef name = callee->getName();

      if (name.contains("__kmpc_fork_call")) {
        for (const User *U : func.users()) {
          if (const auto *call_inst = dyn_cast<CallBase>(U)) {
            const Function *inner_callee = call_inst->getCalledFunction();
            if (inner_callee &&
                inner_callee->getName().contains("__kmpc_fork_call")) {
              ConcurrencyBugReport report(
                  ConcurrencyBugType::OMP_NESTED_PARALLEL_DISABLED,
                  "Nested parallel region may not execute - "
                  "OMP_MAX_ACTIVE_LEVELS might be 1",
                  BugDescription::BI_LOW, BugDescription::BC_WARNING);
              report.addStep(&inst, "Nested parallel region detected");
              reports.push_back(std::move(report));
              break;
            }
          }
        }
      }
    }
  }

  return reports;
}

std::vector<ConcurrencyBugReport>
OpenMPChecker::checkSharedPrivateConflict() const {
  std::vector<ConcurrencyBugReport> reports;
  return reports;
}

std::vector<ConcurrencyBugReport> OpenMPChecker::checkIfFalseParallel() const {
  std::vector<ConcurrencyBugReport> reports;
  return reports;
}

std::vector<ConcurrencyBugReport>
OpenMPChecker::checkOrderedDependency() const {
  std::vector<ConcurrencyBugReport> reports;
  return reports;
}

std::vector<ConcurrencyBugReport>
OpenMPChecker::checkLastprivateMissing() const {
  std::vector<ConcurrencyBugReport> reports;
  return reports;
}

std::vector<ConcurrencyBugReport> OpenMPChecker::checkCopyinNotShared() const {
  std::vector<ConcurrencyBugReport> reports;
  return reports;
}

std::vector<ConcurrencyBugReport>
OpenMPChecker::checkBarrierInCritical() const {
  std::vector<ConcurrencyBugReport> reports;
  return reports;
}

std::vector<ConcurrencyBugReport> OpenMPChecker::checkPrivateInLoop() const {
  std::vector<ConcurrencyBugReport> reports;
  return reports;
}

std::vector<ConcurrencyBugReport> OpenMPChecker::checkMissingSchedule() const {
  std::vector<ConcurrencyBugReport> reports;
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
  append(checkDetachedTaskLeak());
  append(checkNestedSingle());
  append(checkNowaitMissingBarrier());
  append(checkMissingFlush());
  append(checkIncorrectNumThreads());
  append(checkReductionError());
  append(checkTaskwaitMissing());
  append(checkNestedParallelDisabled());
  append(checkSharedPrivateConflict());
  append(checkIfFalseParallel());
  append(checkOrderedDependency());
  append(checkLastprivateMissing());
  append(checkCopyinNotShared());
  append(checkBarrierInCritical());
  append(checkPrivateInLoop());
  append(checkMissingSchedule());

  return reports;
}

} // namespace concurrency