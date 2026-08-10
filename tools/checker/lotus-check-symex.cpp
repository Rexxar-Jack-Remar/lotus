//===- lotus-check-symex.cpp -- Symbolic Execution Bug Checker ------------===//
//
// Lotus frontend for the SymbolicExecution engine. The tool parses LLVM IR,
// runs the legacy SymbolicExecutionWrapper module pass, and emits findings via
// the shared BugReportMgr reporting pipeline.
//
//===----------------------------------------------------------------------===//

#include "SymbolicExecution/SymbolicExecutionWrapper.h"
#include "Alias/InclusionBased/LotusAA/Engine/InterProceduralPass.h"
#include "Checker/Report/BugReportMgr.h"
#include "Checker/Report/ReportOptions.h"
#include "Checker/Report/SuppressionManager.h"
#include "Checker/Tooling/CheckerSubcommands.h"
#include "CheckerReport.h"
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
    return lotus::checker::tooling::EXIT_ERROR;
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

  const int reportStatus = lotus::checker::tooling::emitCheckerReports(
      mgr, {VerboseReports});

  outs() << "\n=== Analysis Complete ===\n";
  return reportStatus;
}
