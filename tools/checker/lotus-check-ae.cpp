//===- lotus-check-ae.cpp -- Abstract Execution Bug Checker --------//
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
#include "Checker/Tooling/CheckerSubcommands.h"
#include "CheckerReport.h"

#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;

static cl::opt<std::string> InputFilename(cl::Positional,
                                          cl::desc("<input bitcode file>"),
                                          cl::Required,
                                          cl::sub(lotus::checker::tooling::aeSubCommand()));

static cl::opt<bool> BufferOverflowCheck("overflow",
                                         cl::desc("Check for buffer overflows"),
                                         cl::init(false),
                                         cl::sub(lotus::checker::tooling::aeSubCommand()));

static cl::opt<bool>
    NullDerefCheck("null-deref",
                   cl::desc("Check for null pointer dereferences"),
                   cl::init(false),
                   cl::sub(lotus::checker::tooling::aeSubCommand()));

static cl::opt<bool>
    UseAfterFreeCheck("use-after-free",
                      cl::desc("Check for use-after-free bugs"),
                      cl::init(false),
                      cl::sub(lotus::checker::tooling::aeSubCommand()));

static cl::opt<bool> InvalidFreeCheck("invalid-free",
                                      cl::desc("Check for invalid free bugs"),
                                      cl::init(false),
                                      cl::sub(lotus::checker::tooling::aeSubCommand()));

static cl::opt<bool> MemLeakCheck("mem-leak",
                                  cl::desc("Check for memory leaks"),
                                  cl::init(false),
                                  cl::sub(lotus::checker::tooling::aeSubCommand()));

static cl::opt<bool>
    AllChecks("all",
              cl::desc("Run all checkers (overflow, null-deref, "
                       "use-after-free, invalid-free, mem-leak)"),
              cl::init(false),
              cl::sub(lotus::checker::tooling::aeSubCommand()));

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
        cl::init(lotus::analysis::AbstractInterpretation::WIDEN_NARROW),
        cl::sub(lotus::checker::tooling::aeSubCommand()));

static cl::opt<unsigned>
    WidenDelayOpt("widen-delay", cl::desc("Delay widening for loop iterations"),
                  cl::init(3),
                  cl::sub(lotus::checker::tooling::aeSubCommand()));

static cl::opt<bool>
    StrictCheckpointOpt("strict-checkpoint",
                        cl::desc("Fail when checkpoints remain unchecked"),
                        cl::init(true),
                        cl::sub(lotus::checker::tooling::aeSubCommand()));
static cl::opt<bool>
    VerboseReports("v",
                   cl::desc("Print trace and IR details for reported bugs"),
                   cl::init(false),
                   cl::sub(lotus::checker::tooling::aeSubCommand()));

int runAECheckerTool(const char *argv0) {
  LLVMContext Context;
  SMDiagnostic Err;
  std::unique_ptr<Module> M = parseIRFile(InputFilename, Err, Context);
  if (!M) {
    Err.print(argv0, errs());
    return lotus::checker::tooling::EXIT_ERROR;
  }

  // Determine which checkers to run
  const bool useDefaults =
      !BufferOverflowCheck && !NullDerefCheck && !UseAfterFreeCheck &&
      !InvalidFreeCheck && !MemLeakCheck && !AllChecks;
  const bool runOverflow = BufferOverflowCheck || AllChecks || useDefaults;
  const bool runNullDeref = NullDerefCheck || AllChecks || useDefaults;
  const bool runUseAfterFree = UseAfterFreeCheck || AllChecks || useDefaults;
  const bool runInvalidFree = InvalidFreeCheck || AllChecks || useDefaults;
  const bool runMemLeak = MemLeakCheck || AllChecks || useDefaults;

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

  const int reportStatus = lotus::checker::tooling::emitCheckerReports(
      mgr, {VerboseReports});
  if (reportStatus != lotus::checker::tooling::EXIT_SUCCESS_CODE) {
    return reportStatus;
  }

  outs() << "\n=== Analysis Complete ===\n";

  return lotus::checker::tooling::EXIT_SUCCESS_CODE;
}
