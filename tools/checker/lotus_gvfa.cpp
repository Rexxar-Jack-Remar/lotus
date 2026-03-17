// Global Value Flow Analysis Tool
// Unified tool for vulnerability detection (null pointer, use-after-free, etc.)
// Integrates GVFA with optional NullCheckAnalysis for improved precision

#include "Alias/DyckAA/DyckAliasAnalysis.h"
#include "Alias/DyckAA/DyckModRefAnalysis.h"
#include "Analysis/GVFA/GlobalValueFlowAnalysis.h"
#include "Analysis/NullPointer/ContextSensitiveNullCheckAnalysis.h"
#include "Analysis/NullPointer/NullCheckAnalysis.h"
#include "Checker/GVFA/FreeOfNonHeapMemoryChecker.h"
#include "Checker/GVFA/InvalidUseOfStackAddressChecker.h"
#include "Checker/GVFA/NullPointerChecker.h"
#include "Checker/GVFA/UseAfterFreeChecker.h"
#include "Checker/GVFA/UseOfUninitializedVariableChecker.h"
#include "Checker/Report/BugReportMgr.h"
#include "Checker/Report/ReportOptions.h"
#include "Checker/Report/SuppressionManager.h"
#include "Fuzzing/TargetGeneration.h"
#include "Utils/LLVM/RecursiveTimer.h"

#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/ManagedStatic.h>
#include <llvm/Support/PrettyStackTrace.h>
#include <llvm/Support/Signals.h>
#include <llvm/Support/SourceMgr.h>

using namespace llvm;

static cl::opt<std::string> InputFilename(cl::Positional,
                                          cl::desc("<input bitcode file>"),
                                          cl::Required);
static cl::opt<std::string>
    VulnType("vuln-type",
             cl::desc("Vulnerability type:\n"
                      "  nullpointer - Null pointer dereference\n"
                      "  useafterfree - Use after free\n"
                      "  uninitialized - Use of uninitialized variable\n"
                      "  freenonheap - Free of non-heap memory\n"
                      "  stackaddress - Invalid use of stack address"),
             cl::init("nullpointer"));
static cl::opt<bool>
    UseNPA("use-npa", cl::desc("Use NullCheckAnalysis to improve precision"),
           cl::init(false));
static cl::opt<bool>
    ContextSensitive("ctx", cl::desc("Use context-sensitive analysis"),
                     cl::init(false));
static cl::opt<bool>
    Verbose("verbose", cl::desc("Print detailed vulnerability information"),
            cl::init(false));
static cl::opt<bool> DumpStats("dump-stats",
                               cl::desc("Dump analysis statistics"),
                               cl::init(false));
static cl::opt<bool>
    ShowProgress("show-progress",
                 cl::desc("Show analysis progress for large-scale detection"),
                 cl::init(false));
static cl::opt<std::string> JsonOutput("json-output",
                                       cl::desc("Output JSON report to file"),
                                       cl::init(""));
static cl::opt<int> MinScore("min-score",
                             cl::desc("Minimum confidence score for reporting"),
                             cl::init(0));

int main(int argc, char **argv) {
  sys::PrintStackTraceOnErrorSignal(argv[0]);
  PrettyStackTraceProgram X(argc, argv);
  llvm_shutdown_obj Y;
  cl::ParseCommandLineOptions(argc, argv, "Global Value Flow Analysis Tool\n");

  // Load module
  LLVMContext Context;
  SMDiagnostic Err;
  std::unique_ptr<Module> M = parseIRFile(InputFilename, Err, Context);
  if (!M) {
    Err.print(argv[0], errs());
    return 1;
  }

  outs() << "Module: " << M->getModuleIdentifier() << " ("
         << M->getFunctionList().size() << " functions)\n";

  // Run prerequisite analyses
  legacy::PassManager PM;
  auto *DyckAA = new DyckAliasAnalysis();
  auto *DyckMRA = new DyckModRefAnalysis();
  PM.add(DyckAA);
  PM.add(DyckMRA);

  NullCheckAnalysis *NCA = nullptr;
  ContextSensitiveNullCheckAnalysis *CSNCA = nullptr;
  if (UseNPA && VulnType == "nullpointer") {
    if (ContextSensitive) {
      PM.add(CSNCA = new ContextSensitiveNullCheckAnalysis());
    } else {
      PM.add(NCA = new NullCheckAnalysis());
    }
  }
  PM.run(*M);

  // Setup GVFA
  DyckVFG VFG(DyckAA, DyckMRA, M.get());
  DyckGlobalValueFlowAnalysis GVFA(M.get(), &VFG, DyckAA, DyckMRA);

  // Create and configure vulnerability checker
  std::unique_ptr<GVFAVulnerabilityChecker> checker;
  if (VulnType == "nullpointer") {
    auto npChecker = std::make_unique<NullPointerChecker>();
    if (NCA)
      npChecker->setNullCheckAnalysis(NCA);
    if (CSNCA)
      npChecker->setContextSensitiveNullCheckAnalysis(CSNCA);
    checker = std::move(npChecker);
  } else if (VulnType == "useafterfree") {
    checker = std::make_unique<UseAfterFreeChecker>();
  } else if (VulnType == "uninitialized") {
    checker = std::make_unique<UseOfUninitializedVariableChecker>();
  } else if (VulnType == "freenonheap") {
    checker = std::make_unique<FreeOfNonHeapMemoryChecker>();
  } else if (VulnType == "stackaddress") {
    checker = std::make_unique<InvalidUseOfStackAddressChecker>();
  } else {
    errs() << "Unknown vulnerability type: " << VulnType << "\n";
    errs() << "Available types: nullpointer, useafterfree, uninitialized, "
              "freenonheap, stackaddress\n";
    return 1;
  }

  // Setup GVFA and run analysis
  GVFA.setVulnerabilityChecker(std::move(checker));
  GVFA.run();

  // Detect and report vulnerabilities using high-level API
  auto *VChecker = GVFA.getVulnerabilityChecker();
  {
    RecursiveTimer Timer("DetectAndReport");
    int vulnCount =
        VChecker->detectAndReport(M.get(), &GVFA, ContextSensitive, Verbose);
    outs() << "Found " << vulnCount << " potential vulnerabilities.\n";
  }

  if (DumpStats) {
    outs() << "\n=== Statistics ===\n"
           << "Total queries: " << GVFA.AllQueryCounter << "\n"
           << "Successful queries: " << GVFA.SuccsQueryCounter << "\n";
    if (GVFA.AllQueryCounter > 0) {
      outs() << "Success rate: "
             << (100.0f * static_cast<float>(GVFA.SuccsQueryCounter) /
                 static_cast<float>(GVFA.AllQueryCounter))
             << "%\n";
    }
    GVFA.printOnlineQueryTime(outs(), "Online Query");
  }

  // Post-processing: Suppression and Deduplication
  BugReportMgr &bugMgr = BugReportMgr::get_instance();

  // 1. Load and apply suppressions
  if (!report_options::SuppressionFile.empty()) {
    SuppressionManager suppMgr;
    if (suppMgr.loadFromFile(report_options::SuppressionFile)) {
      bugMgr.setSuppressionManager(&suppMgr);
      bugMgr.filterSuppressed();
      auto stats = suppMgr.getStats();
      outs() << "\nApplied suppressions: " << stats.totalSuppressions
             << " across " << stats.totalFiles << " files\n";
    } else {
      errs() << "Warning: Could not load suppressions from: "
             << report_options::SuppressionFile << "\n";
    }
  }

  // 2. Final deduplication (enhanced algorithm)
  bugMgr.deduplicate_reports(true);

  // 3. Print bug report summary
  outs() << "\n=== Bug Report Summary ===\n";
  {
    RecursiveTimer Timer("PrintBugReportSummary");
    bugMgr.print_summary(outs());
  }

  // 4. Generate JSON report if requested
  int effectiveMinScore =
      std::max(MinScore, report_options::MinConfidenceScore);
  if (!report_options::TargetsOutputFile.empty()) {
    lotus::fuzzing::TargetGenerationOptions options;
    options.min_confidence_score = effectiveMinScore;
    options.include_invalid_reports = report_options::ShowInvalidReports;
    auto findings = lotus::fuzzing::collectFindings(bugMgr, options);
    auto targets = lotus::fuzzing::collectTargets(findings);

    std::string errorMessage;
    if (!lotus::fuzzing::writeTargetsToFile(targets,
                                            report_options::TargetsOutputFile,
                                            &errorMessage)) {
      errs() << "Error writing fuzz targets: " << errorMessage << "\n";
      return 1;
    }
    outs() << "Fuzz targets written to: " << report_options::TargetsOutputFile
           << " (" << targets.size() << " targets)\n";
  }

  if (!JsonOutput.empty() || !report_options::JsonOutputFile.empty()) {
    std::string jsonFile =
        !JsonOutput.empty() ? JsonOutput : report_options::JsonOutputFile;
    std::error_code EC;
    llvm::raw_fd_ostream JsonFile(jsonFile, EC);
    if (EC) {
      errs() << "Error opening JSON output file: " << EC.message() << "\n";
      return 1;
    }
    {
      RecursiveTimer Timer("GenerateJSONReport");
      bugMgr.generate_json_report(JsonFile, effectiveMinScore);
    }
    outs() << "JSON report written to: " << jsonFile << "\n";
  }

  // 5. Generate SARIF report if requested
  if (!report_options::SarifOutputFile.empty()) {
    std::error_code EC;
    llvm::raw_fd_ostream sarif_out(report_options::SarifOutputFile, EC);
    if (EC) {
      errs() << "Error opening SARIF output file: " << EC.message() << "\n";
      return 1;
    }
    {
      RecursiveTimer Timer("GenerateSARIFReport");
      bugMgr.generate_sarif_report(sarif_out, effectiveMinScore);
    }
    outs() << "SARIF report written to: " << report_options::SarifOutputFile
           << "\n";
  }

  return 0;
}
