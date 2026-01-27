/*
 * Pulse Checker Tool
 *
 * A bug finder using biabductive analysis, inspired by Infer's Pulse.
 * Uses UnderApproxAA for must-alias canonicalization.
 */

#include "Checker/Pulse/PulseChecker.h"
#include "Alias/AliasAnalysisWrapper/AliasAnalysisWrapper.h"
#include "Checker/Pulse/PulseLogger.h"

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/PrettyStackTrace.h>
#include <llvm/Support/Signals.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>

#include <memory>
#include <string>

using namespace llvm;
using namespace pulse;

static cl::opt<std::string> InputFile(cl::Positional,
                                      cl::desc("<input bitcode>"),
                                      cl::Required);
static cl::opt<bool> Verbose("v", cl::desc("Verbose output"), cl::init(false));
static cl::opt<std::string> LogLevelOpt("log-level",
                                         cl::desc("Log level: none, error, warning, info, debug, trace"),
                                         cl::init("info"));
static cl::opt<bool> ShowPulseStats("pulse-stats", cl::desc("Show Pulse analysis statistics"), cl::init(true));

int main(int argc, char** argv) {
    sys::PrintStackTraceOnErrorSignal(argv[0]);
    PrettyStackTraceProgram X(argc, argv);
    cl::ParseCommandLineOptions(argc, argv, "Pulse Bug Finder\n");

    // Configure logging
    pulse::LogLevel level = pulse::LogLevel::Info;
    if (LogLevelOpt == "none") level = pulse::LogLevel::None;
    else if (LogLevelOpt == "error") level = pulse::LogLevel::Error;
    else if (LogLevelOpt == "warning") level = pulse::LogLevel::Warning;
    else if (LogLevelOpt == "info") level = pulse::LogLevel::Info;
    else if (LogLevelOpt == "debug") level = pulse::LogLevel::Debug;
    else if (LogLevelOpt == "trace") level = pulse::LogLevel::Trace;
    
    if (Verbose && level < pulse::LogLevel::Debug) {
        level = pulse::LogLevel::Debug;
    }
    
    PulseLogger::setLevel(level);
    PulseLogger::setOutputStream(&errs());
    PulseLogger::resetStats();

    SMDiagnostic Err;
    LLVMContext Context;
    std::unique_ptr<Module> M = parseIRFile(InputFile, Err, Context);

    if (!M) {
        Err.print(argv[0], errs());
        return 1;
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

    PulseLogger::info("Analysis complete");
    return 0;
}
