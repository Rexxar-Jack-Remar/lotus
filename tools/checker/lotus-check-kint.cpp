// Kint: A Bug-Finding Tool for C Programs (Refactored version)

#include "Checker/KINT/Log.h"
#include "Checker/KINT/MKintPass.h"
#include "Checker/KINT/Options.h"
#include "Checker/Report/BugReportMgr.h"
#include "Checker/Report/ReportOptions.h"
#include "Checker/Report/SuppressionManager.h"
#include "Checker/Tooling/CheckerSubcommands.h"
#include "CheckerReport.h"

#include <llvm/IR/PassManager.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Passes/PassPlugin.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Transforms/Scalar/SROA.h>
#include <llvm/Transforms/Utils/Mem2Reg.h>

using namespace llvm;

// Command line options
static cl::opt<std::string>
    InputFilename(cl::Positional, cl::desc("<IR file>"), cl::Required,
                  cl::sub(lotus::checker::tooling::kintSubCommand()));
static cl::opt<bool> VerboseReports(
    "v", cl::desc("Print trace and IR details for reported bugs"),
    cl::init(false), cl::sub(lotus::checker::tooling::kintSubCommand()));

static void buildKintPipeline(ModulePassManager &MPM) {
  MPM.addPass(createModuleToFunctionPassAdaptor(PromotePass()));
  MPM.addPass(createModuleToFunctionPassAdaptor(SROAPass()));
  MPM.addPass(kint::MKintPass());
}

// registering pass (new pass manager).
extern "C" ::llvm::PassPluginLibraryInfo LLVM_ATTRIBUTE_WEAK
llvmGetPassPluginInfo() {
  return {LLVM_PLUGIN_API_VERSION, "MKintPass", "v0.1", [](PassBuilder &PB) {
            PB.registerPipelineParsingCallback(
                [](StringRef Name, ModulePassManager &MPM,
                   ArrayRef<PassBuilder::PipelineElement>) {
                  if (Name == "mkint-pass") {
                    buildKintPipeline(MPM);
                    return true;
                  }
                  return false;
                });
          }};
}

int runKintCheckerTool(const char *argv0) {
  // Initialize command line options
  kint::initializeCommandLineOptions();

  // Configure the logger
  mkint::LogConfig logConfig;
  logConfig.quiet = kint::QuietLogging;
  logConfig.useStderr = kint::StderrLogging;
  logConfig.logFile = kint::LogFile;

  // Convert from command-line LogLevel to mkint::LogLevel
  switch (kint::CurrentLogLevel) {
  case kint::LogLevel::DEBUG:
    logConfig.logLevel = mkint::LogLevel::DEBUG;
    break;
  case kint::LogLevel::INFO:
    logConfig.logLevel = mkint::LogLevel::INFO;
    break;
  case kint::LogLevel::WARNING:
    logConfig.logLevel = mkint::LogLevel::WARNING;
    break;
  case kint::LogLevel::ERROR:
    logConfig.logLevel = mkint::LogLevel::ERROR;
    break;
  case kint::LogLevel::NONE:
    logConfig.logLevel = mkint::LogLevel::NONE;
    logConfig.quiet = true; // Also set quiet mode for backward compatibility
    break;
  default:
    logConfig.logLevel = mkint::LogLevel::WARNING;
    break;
  }

  // If quiet is set manually, override the log level
  if (kint::QuietLogging) {
    logConfig.logLevel = mkint::LogLevel::NONE;
  }

  mkint::Logger::getInstance().configure(logConfig);

  const bool noExplicitCheckerSelection =
      kint::CheckAll.getNumOccurrences() == 0 &&
      kint::CheckIntOverflow.getNumOccurrences() == 0 &&
      kint::CheckDivByZero.getNumOccurrences() == 0 &&
      kint::CheckBadShift.getNumOccurrences() == 0 &&
      kint::CheckArrayOOB.getNumOccurrences() == 0 &&
      kint::CheckDeadBranch.getNumOccurrences() == 0;

  // Match the other checker frontends: no checker flags means run all checks.
  if (kint::CheckAll || noExplicitCheckerSelection) {
    kint::CheckIntOverflow = true;
    kint::CheckDivByZero = true;
    kint::CheckBadShift = true;
    kint::CheckArrayOOB = true;
    kint::CheckDeadBranch = true;
  }

  // Print checker configuration
  MKINT_LOG() << "Checker Configuration:";
  MKINT_LOG() << "  Integer Overflow: "
              << (kint::CheckIntOverflow ? "Enabled" : "Disabled");
  MKINT_LOG() << "  Division by Zero: "
              << (kint::CheckDivByZero ? "Enabled" : "Disabled");
  MKINT_LOG() << "  Bad Shift: "
              << (kint::CheckBadShift ? "Enabled" : "Disabled");
  MKINT_LOG() << "  Array Out of Bounds: "
              << (kint::CheckArrayOOB ? "Enabled" : "Disabled");
  MKINT_LOG() << "  Dead Branch: "
              << (kint::CheckDeadBranch ? "Enabled" : "Disabled");

  // Add performance configuration information
  MKINT_LOG() << "Performance Configuration:";
  MKINT_LOG() << "  Function Timeout: "
              << (kint::FunctionTimeout == 0
                      ? "No limit"
                      : std::to_string(kint::FunctionTimeout) + " seconds");

  // Explicitly selecting an empty checker set is a configuration error.
  if (!kint::CheckIntOverflow && !kint::CheckDivByZero &&
      !kint::CheckBadShift && !kint::CheckArrayOOB && !kint::CheckDeadBranch) {
    errs()
        << "error: no KINT checks selected\n"
        << "hint: use --check-all or enable an individual --check-* option\n";
    return lotus::checker::tooling::EXIT_ERROR;
  }

  // Load the module to analyze
  llvm::LLVMContext Context;
  llvm::SMDiagnostic Err;
  std::unique_ptr<llvm::Module> M;

  M = llvm::parseIRFile(InputFilename, Err, Context);
  if (!M) {
    Err.print(argv0, llvm::errs());
    return lotus::checker::tooling::EXIT_ERROR;
  }

  // Create and run the pass (new pass manager with cross-registered proxies)
  llvm::LoopAnalysisManager LAM;
  llvm::FunctionAnalysisManager FAM;
  llvm::CGSCCAnalysisManager CGAM;
  llvm::ModuleAnalysisManager MAM;
  llvm::PassBuilder PB;

  PB.registerModuleAnalyses(MAM);
  PB.registerCGSCCAnalyses(CGAM);
  PB.registerFunctionAnalyses(FAM);
  PB.registerLoopAnalyses(LAM);
  PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);

  llvm::ModulePassManager MPM;
  buildKintPipeline(MPM);

  // Run analysis pipeline (bugs are automatically reported to BugReportMgr)
  MPM.run(*M, MAM);

  BugReportMgr &mgr = BugReportMgr::get_instance();
  return lotus::checker::tooling::emitCheckerReports(mgr, {VerboseReports});
}
