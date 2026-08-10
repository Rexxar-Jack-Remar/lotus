/*
 * Pulse Checker Tool
 *
 * A bug finder using biabductive analysis, inspired by Infer's Pulse.
 * Uses UnderApproxAA for must-alias canonicalization.
 */

#include "Alias/Infrastructure/AliasAnalysisWrapper/AliasAnalysisWrapper.h"
#include "Checker/Pulse/Checker/PulseChecker.h"
#include "Checker/Pulse/Report/PulseLogger.h"
#include "Checker/Pulse/Report/PulseOptions.h"
#include "Checker/Report/BugReportMgr.h"
#include "Checker/Report/ReportOptions.h"
#include "Checker/Report/SuppressionManager.h"
#include "Checker/Tooling/CheckerSubcommands.h"
#include "CheckerReport.h"

#include <memory>
#include <string>

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;
using namespace pulse;

static cl::opt<std::string>
    InputFile(cl::Positional, cl::desc("<input bitcode>"), cl::Required,
              cl::sub(lotus::checker::tooling::pulseSubCommand()));
static cl::opt<bool>
    Verbose("v", cl::desc("Verbose output"), cl::init(false),
            cl::sub(lotus::checker::tooling::pulseSubCommand()));
static cl::opt<pulse::LogLevel> LogLevelOpt(
    "log-level", cl::desc("Log level"),
    cl::values(clEnumValN(pulse::LogLevel::None, "none", "Disable logging"),
               clEnumValN(pulse::LogLevel::Error, "error", "Errors only"),
               clEnumValN(pulse::LogLevel::Warning, "warning",
                          "Warnings and errors"),
               clEnumValN(pulse::LogLevel::Info, "info", "Informational"),
               clEnumValN(pulse::LogLevel::Debug, "debug", "Debug output"),
               clEnumValN(pulse::LogLevel::Trace, "trace", "Trace output")),
    cl::init(pulse::LogLevel::Info),
    cl::sub(lotus::checker::tooling::pulseSubCommand()));
static cl::opt<bool>
    ShowPulseStats("pulse-stats", cl::desc("Show Pulse analysis statistics"),
                   cl::init(true),
                   cl::sub(lotus::checker::tooling::pulseSubCommand()));
static cl::opt<bool> NoSMT("no-smt",
                           cl::desc("Disable SMT solving (fast mode); do not "
                                    "query Z3 for path satisfiability"),
                           cl::init(false),
                           cl::sub(lotus::checker::tooling::pulseSubCommand()));

int runPulseCheckerTool(const char *argv0) {
  // Configure logging
  pulse::LogLevel level = LogLevelOpt.getValue();

  if (Verbose && level < pulse::LogLevel::Debug) {
    level = pulse::LogLevel::Debug;
  }

  PulseLogger::setLevel(level);
  PulseLogger::setOutputStream(&errs());
  PulseLogger::resetStats();

  pulse::options::setDisableSMT(NoSMT);
  if (NoSMT) {
    PulseLogger::info("Fast mode: SMT solving disabled");
  }

  SMDiagnostic Err;
  LLVMContext Context;
  std::unique_ptr<Module> M = parseIRFile(InputFile, Err, Context);

  if (!M) {
    Err.print(argv0, errs());
    return lotus::checker::tooling::EXIT_ERROR;
  }

  PulseLogger::info("Starting Pulse analysis");
  PulseLogger::info("Module: " + M->getName().str());
  PulseLogger::startTimer("total_analysis");

  auto AA = std::make_unique<lotus::AliasAnalysisWrapper>(
      *M, lotus::AAConfig::UnderApprox());
  if (!AA->isInitialized()) {
    errs() << "error: alias analysis failed to initialize\n";
    PulseLogger::endTimer("total_analysis");
    return lotus::checker::tooling::EXIT_ERROR;
  }

  PulseChecker checker(M.get(), AA.get());

  PulseLogger::startTimer("analysis");
  checker.analyze();
  PulseLogger::endTimer("analysis");

  PulseLogger::endTimer("total_analysis");

  if (ShowPulseStats) {
    PulseLogger::printStats();
  }

  BugReportMgr &mgr = BugReportMgr::get_instance();
  lotus::checker::tooling::CheckerReportOptions reportOptions;
  reportOptions.verbose = Verbose;
  const int reportStatus =
      lotus::checker::tooling::emitCheckerReports(mgr, reportOptions);
  PulseLogger::info("Analysis complete");
  return reportStatus;
}
