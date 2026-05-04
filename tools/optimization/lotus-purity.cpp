#include "Alias/UnificationBased/seadsa/AllocSiteInfo.hh"
#include "Alias/UnificationBased/seadsa/AllocWrapInfo.hh"
#include "Alias/UnificationBased/seadsa/DsaAnalysis.hh"
#include "Alias/UnificationBased/seadsa/DsaLibFuncInfo.hh"
#include "Alias/UnificationBased/seadsa/InitializePasses.hh"
#include "Alias/UnificationBased/seadsa/ShadowMem.hh"
#include "Alias/UnificationBased/seadsa/support/RemovePtrToInt.hh"
#include "Analysis/Purity/ExternalPuritySummaryStore.h"
#include "Analysis/Purity/PurityInferenceDriver.h"

#include <memory>
#include <string>
#include <vector>

#include <llvm/Bitcode/BitcodeWriter.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/InitializePasses.h>
#include <llvm/Pass.h>
#include <llvm/PassRegistry.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/InitLLVM.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/ToolOutputFile.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;

namespace {

static cl::OptionCategory PurityCat("Lotus Purity Tool");

static cl::opt<std::string> InputFilename(cl::Positional,
                                          cl::desc("<input bitcode file>"),
                                          cl::Required, cl::cat(PurityCat));

static cl::opt<std::string>
    SummaryFile("purity-summary-file",
                cl::desc("Path to the external purity summary JSON store"),
                cl::value_desc("filename"), cl::init(""), cl::cat(PurityCat));

static cl::opt<std::string>
    OutputFilename("o", cl::desc("Write the updated module to this file"),
                   cl::value_desc("filename"), cl::init(""),
                   cl::cat(PurityCat));

static cl::opt<bool>
    OutputAssembly("S", cl::desc("Write LLVM assembly instead of bitcode"),
                   cl::init(false), cl::cat(PurityCat));

static cl::opt<std::string> ReportJsonFile(
    "report-json", cl::desc("Write the purity report JSON to this file"),
    cl::value_desc("filename"), cl::init(""), cl::cat(PurityCat));

static cl::opt<bool> ApplyAttributes(
    "apply-attrs",
    cl::desc("Materialize inferred readnone/readonly attributes in the module"),
    cl::init(false), cl::cat(PurityCat));

static cl::opt<bool> IncludeSuggested(
    "include-suggested",
    cl::desc("Use suggested summaries in addition to validated "
             "ones during analysis"),
    cl::init(false), cl::cat(PurityCat));

static cl::list<std::string> InvalidateSummaries(
    "invalidate-summary",
    cl::desc("Mark an external summary as rejected and recompute impacted "
             "functions"),
    cl::ZeroOrMore, cl::cat(PurityCat));

static cl::opt<bool> DisableMemorySSAPreparation(
    "disable-memoryssa-prep",
    cl::desc("Skip the SeaDsa/ShadowMem preparation pipeline before running "
             "purity inference"),
    cl::init(false), cl::cat(PurityCat));

static bool moduleContainsShadowMem(const Module &module) {
  for (const Function &function : module) {
    if (function.isDeclaration()) {
      continue;
    }
    for (const Instruction &inst : instructions(function)) {
      const auto *call = dyn_cast<CallBase>(&inst);
      const Function *callee = call ? call->getCalledFunction() : nullptr;
      if (callee && callee->getName().startswith("shadow.mem")) {
        return true;
      }
    }
  }
  return false;
}

static void initializeLegacyPasses() {
  PassRegistry &registry = *PassRegistry::getPassRegistry();
  initializeCore(registry);
  initializeAnalysis(registry);
  initializeTransformUtils(registry);
  initializeIPO(registry);
  initializeCallGraphWrapperPassPass(registry);
  initializeGlobalsAAWrapperPassPass(registry);
  initializeTargetLibraryInfoWrapperPassPass(registry);
  initializeDominatorTreeWrapperPassPass(registry);
  initializeAssumptionCacheTrackerPass(registry);

  seadsa::initializeAnalysisPasses(registry);
  initializeRemovePtrToIntPass(registry);
  initializeAllocWrapInfoPass(registry);
  initializeDsaLibFuncInfoPass(registry);
  initializeAllocSiteInfoPass(registry);
  initializeDsaAnalysisPass(registry);
  initializeShadowMemPassPass(registry);
  initializeStripShadowMemPassPass(registry);
}

static void addMemorySSAPrerequisites(legacy::PassManager &pm) {
  pm.add(new seadsa::RemovePtrToInt());
  pm.add(new seadsa::AllocWrapInfo());
  pm.add(new seadsa::DsaLibFuncInfo());
  pm.add(new seadsa::AllocSiteInfo());
  pm.add(new seadsa::DsaAnalysis());
  pm.add(seadsa::createShadowMemPass());
}

static bool hasSeaDsaModeOverride(int argc, char **argv) {
  for (int index = 1; index < argc; ++index) {
    StringRef arg(argv[index]);
    if (arg == "--sea-dsa" || arg.startswith("--sea-dsa=")) {
      return true;
    }
  }
  return false;
}

bool writeTextFile(StringRef path, StringRef contents, std::string &error) {
  std::error_code ec;
  raw_fd_ostream os(path, ec, sys::fs::OF_Text);
  if (ec) {
    error = ec.message();
    return false;
  }

  os << contents;
  return true;
}

bool writeModuleToFile(const Module &module, StringRef path, bool asAssembly,
                       std::string &error) {
  std::error_code ec;
  ToolOutputFile out(path, ec, sys::fs::OF_None);
  if (ec) {
    error = ec.message();
    return false;
  }

  if (asAssembly) {
    module.print(out.os(), nullptr);
  } else {
    WriteBitcodeToFile(module, out.os());
  }

  out.keep();
  return true;
}

} // namespace

int main(int argc, char **argv) {
  InitLLVM X(argc, argv);
  initializeLegacyPasses();

  std::vector<const char *> parseArgv;
  parseArgv.reserve(static_cast<size_t>(argc) + 1);
  for (int index = 0; index < argc; ++index) {
    parseArgv.push_back(argv[index]);
  }

  std::string defaultSeaDsaArg;
  if (!hasSeaDsaModeOverride(argc, argv)) {
    // Purity only needs stable ShadowMem instrumentation; the default
    // context-sensitive SeaDsa mode is less robust on large benchmarks.
    defaultSeaDsaArg = "--sea-dsa=ci";
    parseArgv.push_back(defaultSeaDsaArg.c_str());
  }

  cl::ParseCommandLineOptions(
      static_cast<int>(parseArgv.size()), parseArgv.data(),
      "Lotus purity workflow tool for external summaries and reporting\n");

  LLVMContext context;
  SMDiagnostic err;
  std::unique_ptr<Module> module = parseIRFile(InputFilename, err, context);
  if (!module) {
    err.print(argv[0], errs());
    return 1;
  }

  if (verifyModule(*module, &errs())) {
    errs() << "error: module verification failed\n";
    return 1;
  }

  const bool hadShadowMemInput = moduleContainsShadowMem(*module);
  const bool shouldPrepareMemorySSA =
      !DisableMemorySSAPreparation && !hadShadowMemInput;
  if (shouldPrepareMemorySSA) {
    legacy::PassManager preparePM;
    addMemorySSAPrerequisites(preparePM);
    preparePM.run(*module);
  }

  lotus::analysis::purity::ExternalPuritySummaryStore summaryStore;
  if (!SummaryFile.empty() && sys::fs::exists(SummaryFile)) {
    std::string loadError;
    if (!summaryStore.loadFromFile(SummaryFile, loadError)) {
      errs() << "error: failed to load summary store '" << SummaryFile
             << "': " << loadError << "\n";
      return 1;
    }
  }

  lotus::analysis::purity::PurityInferenceDriverOptions options;
  options.includeSuggestedSummaries = IncludeSuggested;
  options.applyAttributes = ApplyAttributes;
  options.invalidatedSummaries.assign(InvalidateSummaries.begin(),
                                      InvalidateSummaries.end());

  lotus::analysis::purity::PurityInferenceDriver driver(options);
  const auto report = driver.run(*module, summaryStore);

  lotus::analysis::purity::printPurityInferenceReport(report, outs());

  if (!ReportJsonFile.empty()) {
    std::string writeError;
    const std::string jsonReport =
        lotus::analysis::purity::renderPurityInferenceReportAsJson(report);
    if (!writeTextFile(ReportJsonFile, jsonReport, writeError)) {
      errs() << "error: failed to write report JSON '" << ReportJsonFile
             << "': " << writeError << "\n";
      return 1;
    }
  }

  if (!SummaryFile.empty()) {
    std::string saveError;
    if (!summaryStore.saveToFile(SummaryFile, saveError)) {
      errs() << "error: failed to save summary store '" << SummaryFile
             << "': " << saveError << "\n";
      return 1;
    }
  }

  if (shouldPrepareMemorySSA) {
    legacy::PassManager stripPM;
    stripPM.add(seadsa::createStripShadowMemPass());
    stripPM.run(*module);
  }

  if (!OutputFilename.empty()) {
    std::string writeError;
    if (!writeModuleToFile(*module, OutputFilename, OutputAssembly,
                           writeError)) {
      errs() << "error: failed to write output module '" << OutputFilename
             << "': " << writeError << "\n";
      return 1;
    }
  }

  return 0;
}
