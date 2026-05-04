//===- lotus-ae.cpp -- Abstract Execution Bug Checker --------------//
//
// Lotus tool for buffer overflow and null pointer dereference detection
// using Abstract Execution. Migrated from SVF's AE engine.
//
//===----------------------------------------------------------------------===//

#include "Checker/AE/AEDetector.h"
#include "Checker/AE/AbstractInterpretation.h"
#include "Checker/Report/BugReportMgr.h"
#include "Checker/Report/ReportOptions.h"
#include "Checker/Report/SuppressionManager.h"
#include "Fuzzing/TargetGeneration.h"

#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/PrettyStackTrace.h>
#include <llvm/Support/Signals.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;

static cl::opt<std::string> InputFilename(cl::Positional,
                                          cl::desc("<input bitcode file>"),
                                          cl::Required);

static cl::opt<bool> BufferOverflowCheck("overflow",
                                         cl::desc("Check for buffer overflows"),
                                         cl::init(false));

static cl::opt<bool>
    NullDerefCheck("null-deref",
                   cl::desc("Check for null pointer dereferences"),
                   cl::init(false));

static cl::opt<bool>
    UseAfterFreeCheck("use-after-free",
                      cl::desc("Check for use-after-free bugs"),
                      cl::init(false));

static cl::opt<bool> InvalidFreeCheck("invalid-free",
                                      cl::desc("Check for invalid free bugs"),
                                      cl::init(false));

static cl::opt<bool> MemLeakCheck("mem-leak",
                                  cl::desc("Check for memory leaks"),
                                  cl::init(false));

static cl::opt<bool>
    AllChecks("all",
              cl::desc("Run all checkers (overflow, null-deref, "
                       "use-after-free, invalid-free, mem-leak)"),
              cl::init(false));

static cl::opt<lotus::analysis::AbstractInterpretation::HandleRecur>
    HandleRecurOpt(
        "handle-recur", cl::desc("Recursion handling mode"),
        cl::values(
            clEnumValN(lotus::analysis::AbstractInterpretation::TOP, "top",
                       "Set recursive stores/returns to TOP"),
            clEnumValN(lotus::analysis::AbstractInterpretation::WIDEN_ONLY,
                       "widen-only", "Widening only on recursion"),
            clEnumValN(lotus::analysis::AbstractInterpretation::WIDEN_NARROW,
                       "widen-narrow", "Widening + narrowing on recursion")),
        cl::init(lotus::analysis::AbstractInterpretation::WIDEN_NARROW));

static cl::opt<unsigned>
    WidenDelayOpt("widen-delay", cl::desc("Delay widening for loop iterations"),
                  cl::init(3));

static cl::opt<bool>
    StrictCheckpointOpt("strict-checkpoint",
                        cl::desc("Fail when checkpoints remain unchecked"),
                        cl::init(true));
static cl::opt<bool>
    VerboseReports("v",
                   cl::desc("Print trace and IR details for reported bugs"),
                   cl::init(false));

int main(int argc, char **argv) {
  sys::PrintStackTraceOnErrorSignal(argv[0]);
  PrettyStackTraceProgram X(argc, argv);
  llvm_shutdown_obj Y;
  report_options::initializeReportOptions();

  cl::ParseCommandLineOptions(
      argc, argv,
      "Abstract Execution Bug Detector\n"
      "  [options] <input-bitcode>\n"
      "  Use --all to run all checkers.\n"
      "  Use --report-json=<file> or --report-sarif=<file> for output.\n");

  LLVMContext Context;
  SMDiagnostic Err;
  std::unique_ptr<Module> M = parseIRFile(InputFilename, Err, Context);
  if (!M) {
    Err.print(argv[0], errs());
    return 1;
  }

  // Determine which checkers to run
  bool runOverflow = BufferOverflowCheck || AllChecks;
  bool runNullDeref = NullDerefCheck || AllChecks;
  bool runUseAfterFree = UseAfterFreeCheck || AllChecks;
  bool runInvalidFree = InvalidFreeCheck || AllChecks;
  bool runMemLeak = MemLeakCheck || AllChecks;

  // If no specific checkers are enabled, default to all
  if (!runOverflow && !runNullDeref && !runUseAfterFree && !runInvalidFree &&
      !runMemLeak) {
    runOverflow = true;
    runNullDeref = true;
    runUseAfterFree = true;
    runInvalidFree = true;
  }

  // Run AE analysis
  lotus::analysis::AbstractInterpretation &ae =
      lotus::analysis::AbstractInterpretation::getAEInstance();
  ae.setRecursionMode(HandleRecurOpt);
  ae.setWidenDelay(WidenDelayOpt);
  ae.setStrictCheckpoint(StrictCheckpointOpt);
  ae.setEnableBufOverflowCheck(runOverflow);
  ae.setEnableNullDerefCheck(runNullDeref);
  ae.setEnableMemLeakCheck(runMemLeak);

  // Add detectors based on options
  if (runOverflow) {
    ae.addDetector(std::make_unique<lotus::analysis::BufOverflowDetector>());
    outs() << "Running buffer overflow checker...\n";
  }

  if (runNullDeref) {
    ae.addDetector(std::make_unique<lotus::analysis::NullptrDerefDetector>());
    outs() << "Running null pointer dereference checker...\n";
  }

  if (runUseAfterFree) {
    ae.addDetector(std::make_unique<lotus::analysis::UseAfterFreeDetector>());
    outs() << "Running use-after-free checker...\n";
  }

  if (runInvalidFree) {
    ae.addDetector(std::make_unique<lotus::analysis::InvalidFreeDetector>());
    outs() << "Running invalid free checker...\n";
  }

  if (runMemLeak) {
    ae.addDetector(std::make_unique<lotus::analysis::MemLeakDetector>());
    outs() << "Running memory leak checker...\n";
  }

  // Run the analysis
  ae.runOnModule(M.get());

  BugReportMgr &mgr = BugReportMgr::get_instance();

  if (!report_options::SuppressionFile.empty()) {
    SuppressionManager suppMgr;
    if (suppMgr.loadFromFile(report_options::SuppressionFile)) {
      mgr.setSuppressionManager(&suppMgr);
      mgr.filterSuppressed();
    } else {
      errs() << "Warning: Could not load suppressions from: "
             << report_options::SuppressionFile << "\n";
    }
  }

  mgr.deduplicate_reports(true);
  mgr.print_detailed_reports(outs(), VerboseReports,
                             report_options::MinConfidenceScore,
                             report_options::ShowInvalidReports);

  if (!report_options::TargetsOutputFile.empty()) {
    lotus::fuzzing::TargetGenerationOptions options;
    options.min_confidence_score = report_options::MinConfidenceScore;
    options.include_invalid_reports = report_options::ShowInvalidReports;
    auto findings = lotus::fuzzing::collectFindings(mgr, options);
    auto targets = lotus::fuzzing::collectTargets(findings);

    std::string errorMessage;
    if (!lotus::fuzzing::writeTargetsToFile(
            targets, report_options::TargetsOutputFile, &errorMessage)) {
      errs() << "Error writing fuzz targets: " << errorMessage << "\n";
      return 1;
    }
  }

  if (!report_options::JsonOutputFile.empty()) {
    std::error_code EC;
    raw_fd_ostream json_out(report_options::JsonOutputFile, EC,
                            sys::fs::OF_None);
    if (!EC) {
      mgr.generate_json_report(json_out, report_options::MinConfidenceScore);
    } else {
      errs() << "Error writing JSON report: " << EC.message() << "\n";
    }
  }

  if (!report_options::SarifOutputFile.empty()) {
    std::error_code EC;
    raw_fd_ostream sarif_out(report_options::SarifOutputFile, EC,
                             sys::fs::OF_None);
    if (!EC) {
      mgr.generate_sarif_report(sarif_out, report_options::MinConfidenceScore);
    } else {
      errs() << "Error writing SARIF report: " << EC.message() << "\n";
    }
  }

  outs() << "\n=== Analysis Complete ===\n";

  return 0;
}
