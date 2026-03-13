// Author: rainoftime

#include "Checker/Concurrency/ConcurrencyChecker.h"

#include "Alias/AliasAnalysisWrapper/AliasAnalysisWrapper.h"
#include "Analysis/Concurrency/MHP/HappensBeforeAnalysis.h"
#include "Analysis/Concurrency/Utils/ThreadAPI.h"
#include "Checker/Concurrency/ConcurrencyAnalysisDumper.h"

#include <llvm/Analysis/CallGraph.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Operator.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;
using namespace mhp;
using namespace lotus;

namespace concurrency {

ConcurrencyChecker::ConcurrencyChecker(Module &module)
    : m_module(module), m_aliasAnalysis(nullptr),
      m_threadAPI(ThreadAPI::getThreadAPI()), m_stats{} {

  // Register bug types with BugReportMgr (Clearblue pattern)
  BugReportMgr &mgr = BugReportMgr::get_instance();
  m_dataRaceTypeId =
      mgr.register_bug_type("Data Race", BugDescription::BI_HIGH,
                            BugDescription::BC_SECURITY, "CWE-362");
  m_deadlockTypeId =
      mgr.register_bug_type("Deadlock", BugDescription::BI_HIGH,
                            BugDescription::BC_ERROR, "Deadlock potential");
  m_atomicityViolationTypeId = mgr.register_bug_type(
      "Atomicity Violation", BugDescription::BI_MEDIUM,
      BugDescription::BC_ERROR, "Non-atomic operation sequence");
  m_condVarMisuseTypeId = mgr.register_bug_type(
      "Condition Variable Misuse", BugDescription::BI_HIGH,
      BugDescription::BC_ERROR, "Improper condition variable usage");
  m_lockMismatchTypeId = mgr.register_bug_type(
      "Lock Mismatch", BugDescription::BI_HIGH, BugDescription::BC_ERROR,
      "Lock acquisition/release mismatch");
  m_openMPTaskgroupMismatchTypeId = mgr.register_bug_type(
      "OpenMP Taskgroup Mismatch", BugDescription::BI_HIGH,
      BugDescription::BC_ERROR, "Unbalanced OpenMP taskgroup region");
  m_openMPAtomicMismatchTypeId = mgr.register_bug_type(
      "OpenMP Atomic Region Mismatch", BugDescription::BI_HIGH,
      BugDescription::BC_ERROR, "Unbalanced OpenMP atomic region");
  m_openMPPartialSyncTypeId = mgr.register_bug_type(
      "OpenMP Partial Task Synchronization", BugDescription::BI_MEDIUM,
      BugDescription::BC_ERROR,
      "Selective task wait may leave sibling tasks unsynchronized");
  m_mpiOrphanedRequestTypeId = mgr.register_bug_type(
      "MPI Orphaned Request", BugDescription::BI_HIGH,
      BugDescription::BC_ERROR,
      "Non-blocking MPI request without matching completion");
  m_mpiDeadlockTypeId = mgr.register_bug_type(
      "MPI Deadlock", BugDescription::BI_HIGH, BugDescription::BC_ERROR,
      "Potential blocking communication deadlock");
  m_mpiCollectiveMismatchTypeId = mgr.register_bug_type(
      "MPI Collective Mismatch", BugDescription::BI_HIGH,
      BugDescription::BC_ERROR,
      "Incompatible collective operations across processes");
  m_mpiConditionalCollectiveTypeId = mgr.register_bug_type(
      "MPI Conditional Collective", BugDescription::BI_HIGH,
      BugDescription::BC_ERROR,
      "Collective may be executed only by a subset of ranks");
  m_mpiUnsyncRMATypeId = mgr.register_bug_type(
      "MPI Unsynchronized RMA", BugDescription::BI_HIGH,
      BugDescription::BC_ERROR,
      "RMA operation without recognized synchronization");
  m_mpiRMARaceTypeId = mgr.register_bug_type(
      "MPI RMA Race", BugDescription::BI_HIGH, BugDescription::BC_ERROR,
      "Conflicting RMA operations without sufficient synchronization");
  m_mpiWindowLeakTypeId = mgr.register_bug_type(
      "MPI Window Leak", BugDescription::BI_MEDIUM,
      BugDescription::BC_ERROR, "RMA window may not be freed");

  m_stats.totalInstructions = 0;
  m_stats.mhpPairs = 0;
  m_stats.locksAnalyzed = 0;
  m_stats.dataRacesFound = 0;
  m_stats.deadlocksFound = 0;
  m_stats.atomicityViolationsFound = 0;
  m_stats.condVarBugsFound = 0;
  m_stats.lockMismatchesFound = 0;
  m_stats.openMPBugsFound = 0;
  m_stats.mpiBugsFound = 0;

  for (Function &func : module) {
    if (!func.isDeclaration()) {
      for (inst_iterator I = inst_begin(func), E = inst_end(func); I != E; ++I)
        m_stats.totalInstructions++;
    }
  }
}

void ConcurrencyChecker::runAnalyses() {
  // Config-driven activation: run only analyses required by enabled checks
  // (Goblint-style)
  bool needMHP = m_checkDataRaces || m_checkDeadlocks ||
                 m_checkAtomicityViolations || m_checkCondVars;
  bool needLockSet = m_checkDataRaces || m_checkDeadlocks ||
                     m_checkAtomicityViolations || m_checkCondVars ||
                     m_checkLockMismatches;
  bool needEscape = m_checkDataRaces;
  bool needHappensBefore = m_checkDataRaces;
  bool needOpenMP = m_checkOpenMP;
  bool needMPI = m_checkMPI;

  if (needMHP) {
    m_mhpAnalysis = std::make_unique<MHPAnalysis>(m_module);
    m_mhpAnalysis->enableLockSetAnalysis();
    m_mhpAnalysis->analyze();
    m_stats.mhpPairs = m_mhpAnalysis->getStatistics().num_mhp_pairs;
  }

  // Prefer reusing the lockset computed inside MHPAnalysis (avoids duplicate
  // work).
  m_locksetAnalysisView = nullptr;
  if (needMHP && needLockSet && m_mhpAnalysis &&
      m_mhpAnalysis->getLockSetAnalysis()) {
    m_locksetAnalysisView = m_mhpAnalysis->getLockSetAnalysis();
    m_stats.locksAnalyzed = m_locksetAnalysisView->getStatistics().num_locks;
  } else if (needLockSet) {
    m_locksetAnalysis = std::make_unique<LockSetAnalysis>(m_module);
    if (m_aliasAnalysis)
      m_locksetAnalysis->setAliasAnalysis(m_aliasAnalysis);
    llvm::CallGraph cg(m_module);
    m_locksetAnalysis->setCallGraph(&cg);
    m_locksetAnalysis->analyze();
    m_locksetAnalysisView = m_locksetAnalysis.get();
    m_stats.locksAnalyzed = m_locksetAnalysisView->getStatistics().num_locks;
  }

  if (needEscape) {
    m_escapeAnalysis = std::make_unique<EscapeAnalysis>(m_module);
    m_escapeAnalysis->analyze();
  }

  if (needHappensBefore && m_mhpAnalysis) {
    m_happensBeforeAnalysis =
        std::make_unique<HappensBeforeAnalysis>(m_module, *m_mhpAnalysis);
    lotus::AliasAnalysisWrapper *aa = m_aliasAnalysis;
    if (!aa)
      aa = m_mhpAnalysis->getAliasAnalysis();
    if (aa)
      m_happensBeforeAnalysis->setAliasAnalysis(aa);
    m_happensBeforeAnalysis->analyze();
  }

  if (needOpenMP) {
    m_openMPTaskGraph = std::make_unique<OpenMP::OpenMPTaskGraph>(m_module);
    m_openMPTaskGraph->analyze();
  }

  if (needMPI) {
    m_mpiAnalysis = std::make_unique<mpi::MPIAnalysis>(m_module);
    m_mpiAnalysis->runAnalysis();
  }

  lotus::AliasAnalysisWrapper *aa = m_aliasAnalysis;
  if (!aa && m_mhpAnalysis)
    aa = m_mhpAnalysis->getAliasAnalysis();

  m_dataRaceChecker = std::make_unique<DataRaceChecker>(
      m_module, m_mhpAnalysis.get(), m_locksetAnalysisView,
      m_escapeAnalysis.get(), aa, m_happensBeforeAnalysis.get());
  m_deadlockChecker = std::make_unique<DeadlockChecker>(
      m_module, m_locksetAnalysisView, m_mhpAnalysis.get(), m_threadAPI);
  m_atomicityChecker = std::make_unique<AtomicityChecker>(
      m_module, m_mhpAnalysis.get(), m_locksetAnalysisView, m_threadAPI, aa);
  m_condVarChecker = std::make_unique<ConditionVariableChecker>(
      m_module, m_threadAPI, m_locksetAnalysisView);
  m_lockMismatchChecker = std::make_unique<LockMismatchChecker>(
      m_module, m_locksetAnalysisView, m_threadAPI);
  m_openMPChecker =
      std::make_unique<OpenMPChecker>(m_module, m_openMPTaskGraph.get(), m_threadAPI);
  m_mpiChecker = std::make_unique<MPIChecker>(m_module, m_mpiAnalysis.get());
}

void ConcurrencyChecker::runChecks() {
  if (m_checkDataRaces) {
    checkDataRaces();
  }

  if (m_checkDeadlocks) {
    checkDeadlocks();
  }

  if (m_checkAtomicityViolations) {
    checkAtomicityViolations();
  }

  if (m_checkCondVars) {
    checkConditionVariables();
  }

  if (m_checkLockMismatches) {
    checkLockMismatches();
  }

  if (m_checkOpenMP) {
    checkOpenMPBugs();
  }

  if (m_checkMPI) {
    checkMPIBugs();
  }
}

void ConcurrencyChecker::checkDataRaces() {
  if (m_dataRaceChecker && m_mhpAnalysis) {
    auto reports = m_dataRaceChecker->checkDataRaces();
    m_stats.dataRacesFound = reports.size();
    for (const auto &report : reports) {
      reportBug(report, m_dataRaceTypeId);
    }
  }
}

void ConcurrencyChecker::checkDeadlocks() {
  if (m_deadlockChecker && m_mhpAnalysis && m_locksetAnalysisView) {
    auto reports = m_deadlockChecker->checkDeadlocks();
    m_stats.deadlocksFound = reports.size();
    for (const auto &report : reports) {
      reportBug(report, m_deadlockTypeId);
    }
  }
}

void ConcurrencyChecker::checkAtomicityViolations() {
  if (m_atomicityChecker && m_mhpAnalysis && m_locksetAnalysisView) {
    auto reports = m_atomicityChecker->checkAtomicityViolations();
    m_stats.atomicityViolationsFound = reports.size();
    for (const auto &report : reports) {
      reportBug(report, m_atomicityViolationTypeId);
    }
  }
}

void ConcurrencyChecker::checkConditionVariables() {
  if (m_condVarChecker && m_locksetAnalysisView) {
    auto reports = m_condVarChecker->checkConditionVariables();
    m_stats.condVarBugsFound = reports.size();
    for (const auto &report : reports) {
      reportBug(report, m_condVarMisuseTypeId);
    }
  }
}

void ConcurrencyChecker::checkLockMismatches() {
  if (m_lockMismatchChecker && m_locksetAnalysisView) {
    auto reports = m_lockMismatchChecker->checkLockMisuse();
    m_stats.lockMismatchesFound = reports.size();
    for (const auto &report : reports) {
      reportBug(report, m_lockMismatchTypeId);
    }
  }
}

void ConcurrencyChecker::checkOpenMPBugs() {
  if (!m_openMPChecker) {
    return;
  }

  auto reports = m_openMPChecker->checkOpenMPBugs();
  m_stats.openMPBugsFound = reports.size();
  for (const auto &report : reports) {
    switch (report.bugType) {
    case ConcurrencyBugType::OPENMP_TASKGROUP_MISMATCH:
      reportBug(report, m_openMPTaskgroupMismatchTypeId);
      break;
    case ConcurrencyBugType::OPENMP_ATOMIC_MISMATCH:
      reportBug(report, m_openMPAtomicMismatchTypeId);
      break;
    case ConcurrencyBugType::OPENMP_PARTIAL_SYNC:
      reportBug(report, m_openMPPartialSyncTypeId);
      break;
    default:
      break;
    }
  }
}

void ConcurrencyChecker::checkMPIBugs() {
  if (!m_mpiChecker) {
    return;
  }

  auto reports = m_mpiChecker->checkMPIBugs();
  m_stats.mpiBugsFound = reports.size();
  for (const auto &report : reports) {
    switch (report.bugType) {
    case ConcurrencyBugType::MPI_ORPHANED_REQUEST:
      reportBug(report, m_mpiOrphanedRequestTypeId);
      break;
    case ConcurrencyBugType::MPI_DEADLOCK:
      reportBug(report, m_mpiDeadlockTypeId);
      break;
    case ConcurrencyBugType::MPI_COLLECTIVE_MISMATCH:
      reportBug(report, m_mpiCollectiveMismatchTypeId);
      break;
    case ConcurrencyBugType::MPI_CONDITIONAL_COLLECTIVE:
      reportBug(report, m_mpiConditionalCollectiveTypeId);
      break;
    case ConcurrencyBugType::MPI_UNSYNC_RMA:
      reportBug(report, m_mpiUnsyncRMATypeId);
      break;
    case ConcurrencyBugType::MPI_RMA_RACE:
      reportBug(report, m_mpiRMARaceTypeId);
      break;
    case ConcurrencyBugType::MPI_WINDOW_LEAK:
      reportBug(report, m_mpiWindowLeakTypeId);
      break;
    default:
      break;
    }
  }
}

void ConcurrencyChecker::reportBug(const ConcurrencyBugReport &bug_report,
                                   int bug_type_id) {
  // Create a new BugReport following Clearblue pattern
  BugReport *report = new BugReport(bug_type_id);

  // Add diagnostic steps showing the concurrency bug trace
  // Enhanced with Infer-inspired features: trace levels, node tags, and access
  // info
  int trace_level = 0;
  for (size_t i = 0; i < bug_report.steps.size(); ++i) {
    const auto &step = bug_report.steps[i];
    if (step.instruction) {
      std::vector<NodeTag> tags;

      // Infer node tags based on instruction type
      if (isa<CallInst>(step.instruction)) {
        tags.push_back(NodeTag::CALL_SITE);
      }

      // Determine access type
      std::string access = "step";
      if (isa<LoadInst>(step.instruction)) {
        access = "load";
      } else if (isa<StoreInst>(step.instruction)) {
        access = "store";
      } else if (isa<CallInst>(step.instruction)) {
        access = "call";
      }

      // Use enhanced append_step with trace level, tags, and access info
      report->append_step(const_cast<Instruction *>(step.instruction),
                          step.description, trace_level, tags, access);

      // Increment trace level for nested calls
      if (isa<CallInst>(step.instruction)) {
        trace_level++;
      }
    }
  }

  // Set confidence score based on importance
  report->set_conf_score(bug_report.importance == BugDescription::BI_HIGH ? 90
                                                                          : 70);

  // Add metadata (Infer-inspired feature)
  report->add_metadata("checker", "ConcurrencyChecker");
  if (bug_report.bugType == ConcurrencyBugType::OPENMP_TASKGROUP_MISMATCH ||
      bug_report.bugType == ConcurrencyBugType::OPENMP_ATOMIC_MISMATCH ||
      bug_report.bugType == ConcurrencyBugType::OPENMP_PARTIAL_SYNC) {
    report->add_metadata("checker", "OpenMPChecker");
  } else if (bug_report.bugType == ConcurrencyBugType::MPI_ORPHANED_REQUEST ||
             bug_report.bugType == ConcurrencyBugType::MPI_DEADLOCK ||
             bug_report.bugType == ConcurrencyBugType::MPI_COLLECTIVE_MISMATCH ||
             bug_report.bugType == ConcurrencyBugType::MPI_CONDITIONAL_COLLECTIVE ||
             bug_report.bugType == ConcurrencyBugType::MPI_UNSYNC_RMA ||
             bug_report.bugType == ConcurrencyBugType::MPI_RMA_RACE ||
             bug_report.bugType == ConcurrencyBugType::MPI_WINDOW_LEAK) {
    report->add_metadata("checker", "MPIChecker");
  }
  report->add_metadata(
      "importance",
      bug_report.importance == BugDescription::BI_HIGH ? "HIGH" : "MEDIUM");

  // Report to the manager with deduplication enabled
  BugReportMgr::get_instance().insert_report(bug_type_id, report, true);
}

void ConcurrencyChecker::dumpAnalysisResults(raw_ostream &os,
                                             bool jsonFormat) const {
  if (!m_mhpAnalysis || !m_locksetAnalysisView) {
    os << "No analysis results (runAnalyses() not run or no analyses "
          "enabled).\n";
    return;
  }
  ConcurrencyAnalysisDumper dumper(m_module, m_mhpAnalysis.get(),
                                   m_locksetAnalysisView,
                                   m_escapeAnalysis.get(), m_threadAPI);
  dumper.dump(os, jsonFormat);
}

} // namespace concurrency
