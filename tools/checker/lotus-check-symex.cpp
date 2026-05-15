//===- lotus-check-symex.cpp -- Symbolic Execution Bug Checker ------------===//
//
// Lotus frontend for the SymbolicExecution engine. The tool parses LLVM IR,
// runs the legacy SymbolicExecutionWrapper module pass, and emits findings via
// the shared BugReportMgr reporting pipeline.
//
//===----------------------------------------------------------------------===//

#include "Analysis/SymbolicExecution/SymbolicExecutionWrapper.h"
#include "Alias/InclusionBased/LotusAA/Engine/InterProceduralPass.h"
#include "Checker/Report/BugReportMgr.h"
#include "Checker/Report/ReportOptions.h"
#include "Checker/Report/SuppressionManager.h"
#include "Checker/Tooling/CheckerSubcommands.h"
#include "Fuzzing/TargetGeneration.h"
#include "IR/GSA/GSA.h"
#include "IR/GVFG/GuardedValueFlowBuilder.h"
#include "IR/GVFG/LotusAdapter.h"

#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/InitializePasses.h>
#include <llvm/PassRegistry.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;

static cl::opt<std::string> InputFilename(cl::Positional,
                                          cl::desc("<input bitcode file>"),
                                          cl::Required,
                                          cl::sub(lotus::checker::tooling::symexSubCommand()));

static cl::opt<bool>
    VerboseReports("v",
                   cl::desc("Print trace and IR details for reported bugs"),
                   cl::init(false),
                   cl::sub(lotus::checker::tooling::symexSubCommand()));

int runSymExCheckerTool(const char *argv0) {
  LLVMContext Context;
  SMDiagnostic Err;
  std::unique_ptr<Module> M = parseIRFile(InputFilename, Err, Context);
  if (!M) {
    Err.print(argv0, errs());
    return 1;
  }

  PassRegistry &Registry = *PassRegistry::getPassRegistry();
  initializeCore(Registry);
  initializeAnalysis(Registry);
  initializeTransformUtils(Registry);
  initializeScalarOpts(Registry);
  initializeIPO(Registry);
  initializeInstCombine(Registry);
  initializeTarget(Registry);

  legacy::PassManager PM;
  PM.add(new gsa::ControlDependenceAnalysisPass());
  PM.add(new gsa::GateAnalysisPass());
  PM.add(new LotusAA());
  PM.add(new lotus::gvfg::GuardedValueFlowGraphBuilderPass());
  PM.add(new lotus::gvfg::LotusGuardedValueFlowAdapterPass());
  PM.add(new SymbolicExecutionWrapper());
  PM.run(*M);

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
