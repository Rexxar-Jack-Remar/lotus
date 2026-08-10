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
static cl::opt<bool> Verbose("v", cl::desc("Verbose output"), cl::init(false),
                             cl::sub(lotus::checker::tooling::pulseSubCommand()));
static cl::opt<std::string>
    LogLevelOpt("log-level",
                cl::desc("Log level: none, error, warning, info, debug, trace"),
                cl::init("info"),
                cl::sub(lotus::checker::tooling::pulseSubCommand()));
static cl::opt<bool> ShowPulseStats("pulse-stats",
                                    cl::desc("Show Pulse analysis statistics"),
                                    cl::init(true),
                                    cl::sub(lotus::checker::tooling::pulseSubCommand()));
static cl::opt<std::string> JsonOutput("json-output",
                                       cl::desc("Output JSON report to file"),
                                       cl::init(""),
                                       cl::sub(lotus::checker::tooling::pulseSubCommand()));
static cl::opt<int>
    MinScore("min-score",
             cl::desc("Minimum confidence score for reporting (0-100)"),
             cl::init(0),
             cl::sub(lotus::checker::tooling::pulseSubCommand()));
static cl::opt<bool> NoSMT("no-smt",
                           cl::desc("Disable SMT solving (fast mode); do not "
                                    "query Z3 for path satisfiability"),
                           cl::init(false),
                           cl::sub(lotus::checker::tooling::pulseSubCommand()));

int runPulseCheckerTool(const char *argv0) {
  // Configure logging
  pulse::LogLevel level = pulse::LogLevel::Info;
  if (LogLevelOpt == "none")
    level = pulse::LogLevel::None;
  else if (LogLevelOpt == "error")
    level = pulse::LogLevel::Error;
  else if (LogLevelOpt == "warning")
    level = pulse::LogLevel::Warning;
  else if (LogLevelOpt == "info")
    level = pulse::LogLevel::Info;
  else if (LogLevelOpt == "debug")
    level = pulse::LogLevel::Debug;
  else if (LogLevelOpt == "trace")
    level = pulse::LogLevel::Trace;

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
  reportOptions.minScore = MinScore;
  reportOptions.jsonOutputOverride = JsonOutput;
  const int reportStatus =
      lotus::checker::tooling::emitCheckerReports(mgr, reportOptions);
  PulseLogger::info("Analysis complete");
  return reportStatus;
}
