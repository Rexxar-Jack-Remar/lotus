#include "Checker/Concurrency/ConcurrencyChecker.h"
#include "Checker/Report/BugReportMgr.h"
#include "Checker/Report/ReportOptions.h"
#include "Checker/Report/SuppressionManager.h"
#include "Checker/Tooling/CheckerSubcommands.h"
#include "CheckerReport.h"

#include <cstddef>
#include <string>

#include <llvm/IR/LLVMContext.h>
#include <llvm/ADT/StringSet.h>
#include <llvm/IR/Module.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;
using namespace concurrency;

static cl::opt<std::string>
    InputFilename(cl::Positional, cl::desc("<input file>"), cl::Required,
                  cl::sub(lotus::checker::tooling::concurrencySubCommand()));
static cl::opt<std::string> ChecksList(
    "checks",
    cl::desc("Comma-separated checks to run: "
             "race,deadlock,atomicity,condvar,lock-mismatch,openmp,mpi "
             "(overrides individual flags)"),
    cl::value_desc("list"),
    cl::sub(lotus::checker::tooling::concurrencySubCommand()));
static cl::opt<bool> EnableDataRaces("check-data-races",
                                     cl::desc("Enable data race detection"),
                                     cl::init(true),
                                     cl::sub(lotus::checker::tooling::concurrencySubCommand()));
static cl::opt<bool> EnableDeadlocks("check-deadlocks",
                                     cl::desc("Enable deadlock detection"),
                                     cl::init(true),
                                     cl::sub(lotus::checker::tooling::concurrencySubCommand()));
static cl::opt<bool>
    EnableAtomicity("check-atomicity",
                    cl::desc("Enable atomicity violation detection"),
                    cl::init(true),
                    cl::sub(lotus::checker::tooling::concurrencySubCommand()));
static cl::opt<bool>
    EnableCondVar("check-condvar",
                  cl::desc("Enable condition variable misuse detection"),
                  cl::init(true),
                  cl::sub(lotus::checker::tooling::concurrencySubCommand()));
static cl::opt<bool> EnableLockMismatch(
    "check-lock-mismatch",
    cl::desc("Enable lock acquisition/release mismatch detection"),
    cl::init(true),
    cl::sub(lotus::checker::tooling::concurrencySubCommand()));
static cl::opt<bool>
    EnableOpenMP("check-openmp", cl::desc("Enable dedicated OpenMP bug checks"),
                 cl::init(true),
                 cl::sub(lotus::checker::tooling::concurrencySubCommand()));
static cl::opt<bool> EnableMPI("check-mpi",
                               cl::desc("Enable dedicated MPI bug checks"),
                               cl::init(true),
                               cl::sub(lotus::checker::tooling::concurrencySubCommand()));
static cl::opt<bool> AnalysisOnly(
    "analysis-only",
    cl::desc("Run analysis only (no bug checking), dump analysis results"),
    cl::init(false),
    cl::sub(lotus::checker::tooling::concurrencySubCommand()));
static cl::opt<bool>
    VerboseReports("v",
                   cl::desc("Print trace and IR details for reported bugs"),
                   cl::init(false),
                   cl::sub(lotus::checker::tooling::concurrencySubCommand()));
static cl::opt<std::string>
    AnalysisJsonOutput("analysis-json",
                       cl::desc("Output analysis results as JSON to specified "
                                "file (requires --analysis-only)"),
                       cl::value_desc("filename"),
                       cl::sub(lotus::checker::tooling::concurrencySubCommand()));

int runConcurrencyCheckerTool(const char *argv0) {
  // Parse the input LLVM IR file
  SMDiagnostic err;
  LLVMContext context;
  std::unique_ptr<Module> module = parseIRFile(InputFilename, err, context);

  if (!module) {
    err.print(argv0, errs());
    return lotus::checker::tooling::EXIT_ERROR;
  }

  outs() << "Analyzing module: " << module->getModuleIdentifier() << "\n";

  ConcurrencyChecker checker(*module);

  // Config-driven activation: --checks=race,deadlock,... overrides individual
  // flags (Goblint-style)
  if (!ChecksList.empty()) {
    StringSet<> requested;
    SmallVector<StringRef, 8> pieces;
    StringRef(ChecksList.getValue()).split(pieces, ',', -1, false);
    StringSet<> valid;
    for (StringRef name : {"race", "deadlock", "atomicity", "condvar",
                           "lock-mismatch", "openmp", "mpi"}) {
      valid.insert(name);
    }
    for (StringRef piece : pieces) {
      StringRef name = piece.trim();
      if (name.empty() || valid.find(name) == valid.end()) {
        errs() << "error: unknown concurrency check: "
               << (name.empty() ? "<empty>" : name) << "\n";
        return lotus::checker::tooling::EXIT_ERROR;
      }
      requested.insert(name);
    }
    checker.enableDataRaceCheck(requested.count("race"));
    checker.enableDeadlockCheck(requested.count("deadlock"));
    checker.enableAtomicityCheck(requested.count("atomicity"));
    checker.enableCondVarCheck(requested.count("condvar"));
    checker.enableLockMismatchCheck(requested.count("lock-mismatch"));
    checker.enableOpenMPCheck(requested.count("openmp"));
    checker.enableMPICheck(requested.count("mpi"));
  } else {
    checker.enableDataRaceCheck(EnableDataRaces);
    checker.enableDeadlockCheck(EnableDeadlocks);
    checker.enableAtomicityCheck(EnableAtomicity);
    checker.enableCondVarCheck(EnableCondVar);
    checker.enableLockMismatchCheck(EnableLockMismatch);
    checker.enableOpenMPCheck(EnableOpenMP);
    checker.enableMPICheck(EnableMPI);
  }

  if (AnalysisOnly) {
    checker.enableDataRaceCheck(true);
    checker.enableDeadlockCheck(true);
    checker.enableAtomicityCheck(true);
    checker.enableCondVarCheck(true);
    checker.enableLockMismatchCheck(true);
    checker.enableOpenMPCheck(true);
    checker.enableMPICheck(true);
  }

  checker.runAnalyses();

  if (AnalysisOnly) {
    outs() << "Running concurrency analyses (analysis-only mode)...\n";
    if (!AnalysisJsonOutput.empty()) {
      // Output to JSON file
      std::error_code EC;
      raw_fd_ostream json_out(AnalysisJsonOutput, EC, sys::fs::OF_None);
      if (!EC) {
        checker.dumpAnalysisResults(json_out, true);
        outs() << "\nAnalysis results written to JSON: " << AnalysisJsonOutput
               << "\n";
      } else {
        errs() << "Error writing analysis JSON: " << EC.message() << "\n";
        return lotus::checker::tooling::EXIT_ERROR;
      }
    } else {
      // Output to stdout in human-readable format
      checker.dumpAnalysisResults(outs(), false);
    }
    return lotus::checker::tooling::EXIT_SUCCESS_CODE;
  }

  outs() << "Running concurrency checks...\n";
  checker.runChecks();

  // Print analysis statistics
  auto stats = checker.getStatistics();
  outs() << "\n=== Concurrency Analysis Statistics ===\n";
  outs() << "Total Instructions: " << stats.totalInstructions << "\n";
  outs() << "MHP Pairs: " << stats.mhpPairs << "\n";
  outs() << "Locks Analyzed: " << stats.locksAnalyzed << "\n";
  outs() << "Data Races Found: " << stats.dataRacesFound << "\n";
  outs() << "Deadlocks Found: " << stats.deadlocksFound << "\n";
  outs() << "Atomicity Violations Found: " << stats.atomicityViolationsFound
         << "\n";
  outs() << "Cond Var Bugs Found: " << stats.condVarBugsFound << "\n";
  outs() << "Lock Mismatches Found: " << stats.lockMismatchesFound << "\n";
  outs() << "OpenMP Bugs Found: " << stats.openMPBugsFound << "\n";
  outs() << "  OpenMP Tasks Tracked: " << stats.openMPSummary.task_count
         << "\n";
  outs() << "  OpenMP Tasks With Dependencies: "
         << stats.openMPSummary.task_with_dependencies_count << "\n";
  outs() << "  OpenMP Included Tasks: "
         << stats.openMPSummary.included_task_count << "\n";
  outs() << "  OpenMP Taskloops: " << stats.openMPSummary.taskloop_count
         << "\n";
  outs() << "  OpenMP Wait Boundaries: "
         << stats.openMPSummary.wait_boundary_count << "\n";
  outs() << "  OpenMP Partial Wait Boundaries: "
         << stats.openMPSummary.partial_wait_boundary_count << "\n";
  outs() << "  OpenMP Taskgroups: "
         << stats.openMPSummary.taskgroup_region_count << "\n";
  outs() << "  OpenMP Worksharing Regions: "
         << stats.openMPSummary.single_region_count +
                stats.openMPSummary.sections_region_count +
                stats.openMPSummary.worksharing_loop_count +
                stats.openMPSummary.reduction_region_count +
                stats.openMPSummary.ordered_region_count +
                stats.openMPSummary.master_region_count
         << "\n";
  outs() << "  OpenMP Atomic Regions: "
         << stats.openMPSummary.atomic_region_count << "\n";
  outs() << "  OpenMP Flushes: " << stats.openMPSummary.flush_count << "\n";
  outs() << "  OpenMP Cancels: " << stats.openMPSummary.cancel_count << "\n";
  outs() << "  OpenMP Target Regions: "
         << stats.openMPSummary.target_region_count +
                stats.openMPSummary.target_data_region_count
         << "\n";
  outs() << "MPI Bugs Found: " << stats.mpiBugsFound << "\n";
  outs() << "  MPI Operations Tracked: " << stats.mpiSummary.operation_count
         << "\n";
  outs() << "  MPI Nonblocking Operations: "
         << stats.mpiSummary.nonblocking_operation_count << "\n";
  outs() << "  MPI Collective Operations: "
         << stats.mpiSummary.collective_operation_count << "\n";
  outs() << "  MPI Communicator Management Ops: "
         << stats.mpiSummary.communicator_management_count << "\n";
  outs() << "  MPI Request Management Ops: "
         << stats.mpiSummary.request_management_count << "\n";
  outs() << "  MPI RMA Data Ops: " << stats.mpiSummary.rma_operation_count
         << "\n";
  outs() << "  MPI RMA Sync Ops: " << stats.mpiSummary.rma_sync_count << "\n";
  outs() << "  MPI Unsynchronized RMA Ops: "
         << stats.mpiSummary.unsynchronized_rma_count << "\n";
  outs() << "  MPI RMA Races: " << stats.mpiSummary.rma_race_count << "\n";
  outs() << "  MPI Leaked Windows: " << stats.mpiSummary.leaked_window_count
         << "\n";
  outs() << "  MPI Collective Slots Tracked: "
         << stats.mpiSummary.collective_slot_count << "\n";

  BugReportMgr &mgr = BugReportMgr::get_instance();
  return lotus::checker::tooling::emitCheckerReports(mgr, {VerboseReports});
}
