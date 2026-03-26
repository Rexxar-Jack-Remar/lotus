//===- lotus-saber.cpp -- Source-sink bug checker (Saber) ------------------//
//
// Lotus tool for memory leak, double-free, and file descriptor checks.
// Mirrors SVF's saber tool; uses Saber engine on Lotus SVFG.
//
//===----------------------------------------------------------------------===//

#include "Checker/Report/BugReportMgr.h"
#include "Checker/Report/ReportOptions.h"
#include "Checker/Report/SuppressionManager.h"
#include "Checker/Saber/DoubleFreeChecker.h"
#include "Checker/Saber/FileChecker.h"
#include "Checker/Saber/LeakChecker.h"
#include "Checker/Saber/SaberOptions.h"
#include "Fuzzing/TargetGeneration.h"
#include "Utils/LLVM/RecursiveTimer.h"

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

static cl::opt<bool> MemoryLeakCheck(
    "leak",
    cl::desc("Check for memory leaks (alloc never freed or partial leak)"),
    cl::init(false));

static cl::opt<bool>
    FileCheck("file",
              cl::desc("Check for file descriptor leaks (fopen never fclose)"),
              cl::init(false));

static cl::opt<bool> DFreeCheck(
    "double-free",
    cl::desc("Check for double-free (same memory freed twice on same path)"),
    cl::init(false));

static cl::opt<bool>
    AllChecks("all", cl::desc("Run all checkers (leak, double-free, file)"),
              cl::init(false));
static cl::opt<bool> VerboseReports(
    "v", cl::desc("Print trace and IR details for reported bugs"),
    cl::init(false));

int main(int argc, char **argv) {
  sys::PrintStackTraceOnErrorSignal(argv[0]);
  PrettyStackTraceProgram X(argc, argv);
  llvm_shutdown_obj Y;
  report_options::initializeReportOptions();

  cl::ParseCommandLineOptions(
      argc, argv,
      "Source-Sink Bug Detector (Saber)\n"
      "  [options] <input-bitcode>\n"
      "  By default, runs leak checker. Use --all to run all checkers.\n"
      "  Use --report-json=<file> or --report-sarif=<file> for output.\n");
  RecursiveTimer::setEnabled(lotus::analysis::SaberOptions::verbose());

  // Force linkage of SaberOptions symbols from static library
  // Reference the extern variables to ensure they're linked
  (void)&lotus::analysis::SaberVerbose;
  (void)&lotus::analysis::SaberFullSVFG;
  (void)&lotus::analysis::SaberCxtLimit;
  (void)&lotus::analysis::SaberMaxStepInWrapper;
  (void)&lotus::analysis::SaberDumpSlice;
  (void)&lotus::analysis::SaberValidateTests;
  (void)&lotus::analysis::SaberCollectExtRetGlobals;

  LLVMContext Context;
  SMDiagnostic Err;
  std::unique_ptr<Module> M = parseIRFile(InputFilename, Err, Context);
  if (!M) {
    Err.print(argv[0], errs());
    return 1;
  }

  // Determine which checkers to run
  const bool anyExplicitChecker =
      MemoryLeakCheck.getNumOccurrences() != 0 ||
      DFreeCheck.getNumOccurrences() != 0 || FileCheck.getNumOccurrences() != 0;
  bool runLeak = false;
  bool runDoubleFree = false;
  bool runFile = false;
  if (AllChecks) {
    runLeak = true;
    runDoubleFree = true;
    runFile = true;
  } else if (anyExplicitChecker) {
    runLeak = MemoryLeakCheck;
    runDoubleFree = DFreeCheck;
    runFile = FileCheck;
  } else {
    runLeak = true;
  }

  // Count how many checkers will run
  int checkerCount = 0;
  if (runLeak)
    checkerCount++;
  if (runDoubleFree)
    checkerCount++;
  if (runFile)
    checkerCount++;

  // If running multiple checkers, build SVFG/ICFG once and share them
  std::unique_ptr<lotus::analysis::SVFG> shared_svfg;
  std::unique_ptr<::ICFG> shared_icfg;
  lotus::analysis::SrcSnkDDA::RemovedSUVFEdges shared_removed_su_vfg_edges;

  if (checkerCount > 1) {
    // Build SVFG/ICFG once using a temporary checker
    outs() << "\n=== Building SVFG (shared across checkers) ===\n";
    lotus::analysis::LeakChecker builderChecker;
    builderChecker.setModule(M.get());
    builderChecker.initialize();
    builderChecker.exportRemovedSUVFEdges(shared_removed_su_vfg_edges);

    // Extract SVFG/ICFG to share (ownership moves to shared_svfg/shared_icfg)
    auto extracted = builderChecker.extractSVFGAndICFG();
    shared_svfg = std::move(extracted.first);
    shared_icfg = std::move(extracted.second);
  }

  // Run each checker
  if (runLeak) {
    if (checkerCount > 1) {
      outs() << "\n=== Running Memory Leak Checker ===\n";
    } else {
      outs() << "Running Memory Leak checker...\n";
    }
    lotus::analysis::LeakChecker leakChecker;
    if (checkerCount > 1 && shared_svfg && shared_icfg) {
      // Move shared graphs into this checker.
      leakChecker.setSharedSVFGAndICFG(std::move(shared_svfg),
                                       std::move(shared_icfg));
      leakChecker.importRemovedSUVFEdges(shared_removed_su_vfg_edges);
    }
    leakChecker.setModule(M.get());
    leakChecker.runOnModule(*M);
    if (checkerCount > 1) {
      // Hand graphs to the next checker only after this one has finished.
      leakChecker.exportRemovedSUVFEdges(shared_removed_su_vfg_edges);
      auto extracted = leakChecker.extractSVFGAndICFG();
      shared_svfg = std::move(extracted.first);
      shared_icfg = std::move(extracted.second);
    }
  }

  if (runDoubleFree) {
    if (checkerCount > 1) {
      outs() << "\n=== Running Double Free Checker ===\n";
    } else {
      outs() << "Running Double Free checker...\n";
    }
    lotus::analysis::DoubleFreeChecker dfChecker;
    if (checkerCount > 1 && shared_svfg && shared_icfg) {
      dfChecker.setSharedSVFGAndICFG(std::move(shared_svfg),
                                     std::move(shared_icfg));
      dfChecker.importRemovedSUVFEdges(shared_removed_su_vfg_edges);
    }
    // Note: Double-free checker uses free() calls as both sources and sinks,
    // so it doesn't share source/sink state with leak checker.
    dfChecker.setModule(M.get());
    dfChecker.runOnModule(*M);
    if (checkerCount > 1) {
      dfChecker.exportRemovedSUVFEdges(shared_removed_su_vfg_edges);
      auto extracted = dfChecker.extractSVFGAndICFG();
      shared_svfg = std::move(extracted.first);
      shared_icfg = std::move(extracted.second);
    }
  }

  if (runFile) {
    if (checkerCount > 1) {
      outs() << "\n=== Running File Descriptor Checker ===\n";
    } else {
      outs() << "Running File Descriptor checker...\n";
    }
    lotus::analysis::FileChecker fileChecker;
    if (checkerCount > 1 && shared_svfg && shared_icfg) {
      fileChecker.setSharedSVFGAndICFG(std::move(shared_svfg),
                                       std::move(shared_icfg));
      fileChecker.importRemovedSUVFEdges(shared_removed_su_vfg_edges);
    }
    fileChecker.setModule(M.get());
    fileChecker.runOnModule(*M);
  }

  // Print bug report summary
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
  if (mgr.get_total_reports() > 0) {
    mgr.print_detailed_reports(outs(), VerboseReports,
                               report_options::MinConfidenceScore,
                               report_options::ShowInvalidReports);
  } else {
    outs() << "\nNo bugs found.\n";
  }

  if (!report_options::TargetsOutputFile.empty()) {
    lotus::fuzzing::TargetGenerationOptions options;
    options.min_confidence_score = report_options::MinConfidenceScore;
    options.include_invalid_reports = report_options::ShowInvalidReports;
    auto findings = lotus::fuzzing::collectFindings(mgr, options);
    auto targets = lotus::fuzzing::collectTargets(findings);

    std::string errorMessage;
    if (!lotus::fuzzing::writeTargetsToFile(targets,
                                            report_options::TargetsOutputFile,
                                            &errorMessage)) {
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

  if (checkerCount > 1) {
    outs() << "\n=== Analysis Complete ===\n";
  }

  return 0;
}
