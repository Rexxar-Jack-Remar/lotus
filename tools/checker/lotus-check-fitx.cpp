/*
 * FiTx Bug Finder Tool (Standalone)
 *
 * FiTx: Framework for Finger Traceable Bugs in Linux
 * A static analysis framework for detecting common memory and concurrency bugs.
 *
 * Supported bug types:
 * - Double Free (df)
 * - Double Lock (dl)
 * - Double Unlock (dul)
 * - Memory Leak (leak)
 * - Null Pointer Dereference (nullptr)
 * - Use After Free (uaf)
 * - Use Before Initialization (ubi)
 * - Reference Count Issues (ref_count, ref_uncount)
 *
 * Usage: lotus-fitx [options] <input bitcode>
 */

#include "Checker/FiTx/Core/Logs.h"
#include "Checker/FiTx/Detector/DF_Detector.h"
#include "Checker/FiTx/Detector/DL_Detector.h"
#include "Checker/FiTx/Detector/DUL_Detector.h"
#include "Checker/FiTx/Detector/Leak_Detector.h"
#include "Checker/FiTx/Detector/NullPtr_Detector.h"
#include "Checker/FiTx/Detector/Ref_Detector.h"
#include "Checker/FiTx/Detector/UAF_Detector.h"
#include "Checker/FiTx/Detector/Unref_Detector.h"
#include "Checker/FiTx/Detector/UseBeforeInit_Detector.h"
#include "Checker/FiTx/Framework_IR/IRGenerator.h"
#include "Checker/FiTx/Frontend/Analyzer.h"
#include "Checker/FiTx/Frontend/Framework.h"
#include "Checker/FiTx/Frontend/State.h"
#include "Checker/Report/BugReportMgr.h"
#include "Checker/Report/ReportOptions.h"
#include "Checker/Report/SuppressionManager.h"
#include "Checker/Tooling/CheckerSubcommands.h"
#include "CheckerReport.h"

#include <chrono>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <llvm/ADT/StringMap.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/InitializePasses.h>
#include <llvm/PassRegistry.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/PrettyStackTrace.h>
#include <llvm/Support/Signals.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;

static cl::opt<std::string>
    InputFile(cl::Positional, cl::desc("<input bitcode>"), cl::Required,
              cl::sub(lotus::checker::tooling::fitxSubCommand()));
static cl::opt<bool> Verbose("v", cl::desc("Verbose output"), cl::init(false),
                             cl::sub(lotus::checker::tooling::fitxSubCommand()));
static cl::opt<bool> MeasureAnalysisTime("fitx-measure",
                                         cl::desc("Measure analysis time"),
                                         cl::init(false),
                                         cl::sub(lotus::checker::tooling::fitxSubCommand()));
static cl::opt<std::string>
    DetectorType("detector",
                 cl::desc("Detector type: all, df, dl, dul, leak, nullptr, "
                          "uaf, ubi, ref_count, ref_uncount"),
                 cl::init("all"),
                 cl::sub(lotus::checker::tooling::fitxSubCommand()));

namespace {
using DetectorDefinition = void (*)(fitx::StateManager &);
using DetectorDefinitions = std::vector<DetectorDefinition>;

class SelectedDetector final : public fitx::FrameworkPass {
public:
  explicit SelectedDetector(DetectorDefinitions definitions)
      : definitions_(std::move(definitions)) {}

  void defineStates() override {
    for (DetectorDefinition define : definitions_) {
      fitx::StateManager manager;
      define(manager);
      addStateManager(std::move(manager));
    }
  }

private:
  DetectorDefinitions definitions_;
};

const StringMap<DetectorDefinitions> &detectorRegistry() {
  static const StringMap<DetectorDefinitions> registry = [] {
    StringMap<DetectorDefinitions> result;
    result["df"] = {DoubleFree::define_states};
    result["dl"] = {DoubleLock::define_states};
    result["dul"] = {DoubleUnlock::defineStates};
    result["leak"] = {MemoryLeak::defineStates};
    result["nullptr"] = {NullPointer::defineStates};
    result["uaf"] = {UseAfterFree::defineStates};
    result["ubi"] = {UseBeforeInitialization::defineStates};
    result["ref_count"] = {ReferenceCounter::defineStates};
    result["ref_uncount"] = {UnreferenceCounter::defineStates};
    result["all"] = {DoubleFree::define_states,
                     DoubleLock::define_states,
                     DoubleUnlock::defineStates,
                     MemoryLeak::defineStates,
                     NullPointer::defineStates,
                     UnreferenceCounter::defineStates,
                     ReferenceCounter::defineStates,
                     UseAfterFree::defineStates,
                     UseBeforeInitialization::defineStates};
    return result;
  }();
  return registry;
}

std::unique_ptr<fitx::FrameworkPass> createDetector(StringRef name) {
  auto detector = detectorRegistry().find(name);
  if (detector == detectorRegistry().end()) {
    return nullptr;
  }
  return std::make_unique<SelectedDetector>(detector->second);
}

} // namespace

namespace fitx {

// Run all registered FiTx checkers via the legacy pass manager.
bool runFiTxAnalysis(Module &M, StringRef detectorName) {
  std::unique_ptr<FrameworkPass> detector = createDetector(detectorName);
  if (!detector) {
    errs() << "error: unknown FiTx detector '" << detectorName << "'\n";
    return false;
  }

  outs() << "FiTx Bug Finder\n";
  outs() << "================\n\n";
  outs() << "Module: " << M.getName() << "\n";
  outs() << "Functions: " << M.size() << "\n";
  outs() << "Detector: " << detectorName << "\n";

  auto start = std::chrono::system_clock::now();

  // Ensure LoopInfoWrapperPass is initialized (required by IRGenerator).
  PassRegistry &Registry = *PassRegistry::getPassRegistry();
  initializeLoopInfoWrapperPassPass(Registry);

  legacy::PassManager PM;

  // 1. Build framework IR for all functions (required by FrameworkPass).
  PM.add(new LoopInfoWrapperPass());
  PM.add(new ir_generator::IRGenerator());

  // 2. Run only the detector selected by the CLI.
  PM.add(detector.release());

  PM.run(M);

  auto end = std::chrono::system_clock::now();
  auto duration =
      std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

  outs() << "Analysis complete.\n";
  if (MeasureAnalysisTime) {
    outs() << "Time: " << duration.count() << " ms\n";
  }
  return true;
}

} // namespace fitx

int runFiTxCheckerTool(const char *argv0) {
  SMDiagnostic Err;
  LLVMContext Context;
  std::unique_ptr<Module> M = parseIRFile(InputFile, Err, Context);

  if (!M) {
    Err.print(argv0, errs());
    return lotus::checker::tooling::EXIT_ERROR;
  }

  if (!fitx::runFiTxAnalysis(*M, DetectorType)) {
    return lotus::checker::tooling::EXIT_ERROR;
  }

  BugReportMgr &mgr = BugReportMgr::get_instance();

  return lotus::checker::tooling::emitCheckerReports(mgr, {Verbose});
}
