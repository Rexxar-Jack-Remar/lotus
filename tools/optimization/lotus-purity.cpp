#include "Analysis/Purity/ExternalPuritySummaryStore.h"
#include "Analysis/Purity/PurityInferenceDriver.h"

#include <memory>
#include <string>

#include <llvm/Bitcode/BitcodeWriter.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/IRReader/IRReader.h>
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

static cl::opt<std::string> SummaryFile(
    "summary-file",
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

static cl::opt<bool>
    IncludeSuggested("include-suggested",
                     cl::desc("Use suggested summaries in addition to validated "
                              "ones during analysis"),
                     cl::init(false), cl::cat(PurityCat));

static cl::list<std::string> InvalidateSummaries(
    "invalidate-summary",
    cl::desc("Mark an external summary as rejected and recompute impacted "
             "functions"),
    cl::ZeroOrMore, cl::cat(PurityCat));

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
  cl::ParseCommandLineOptions(
      argc, argv,
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
