#include "Checker/Core/CheckerDriver.h"
#include "Checker/Core/CheckerRegistry.h"
#include "Checker/Core/CheckerSpecLoader.h"
#include "Checker/Report/BugReportMgr.h"
#include "Checker/Report/ReportOptions.h"
#include "Checker/Report/SuppressionManager.h"

#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/PrettyStackTrace.h>
#include <llvm/Support/Signals.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>

#include <optional>
#include <string>
#include <vector>

using namespace llvm;

static cl::opt<std::string> InputFilename(
    cl::Positional, cl::desc("<input bitcode file>"), cl::init(""));
static cl::opt<bool>
    ListCheckers("list-checkers",
                 cl::desc("List registered checker specs and exit"),
                 cl::init(false));
static cl::opt<std::string>
    CheckerIds("checker", cl::desc("Comma-separated checker ids to run"),
               cl::init(""));
static cl::opt<std::string>
    CategoryFilter("category", cl::desc("Run only checkers in a category"),
                   cl::init(""));
static cl::opt<std::string>
    EngineFilter("engine", cl::desc("Filter by engine kind"), cl::init(""));
static cl::opt<std::string>
    BuiltinSpecDir("spec-dir",
                   cl::desc("Directory containing YAML checker specs"),
                   cl::init("config/checkers"));
static cl::opt<bool> VerboseReports(
    "v", cl::desc("Print trace and IR details for reported bugs"), cl::init(false));

namespace {

std::optional<lotus::checker::EngineKind> parseEngine(StringRef engine) {
  if (engine.empty()) {
    return std::nullopt;
  }
  if (engine == "declarative") {
    return lotus::checker::EngineKind::Declarative;
  }
  if (engine == "ae") {
    return lotus::checker::EngineKind::AE;
  }
  if (engine == "saber") {
    return lotus::checker::EngineKind::Saber;
  }
  if (engine == "pulse") {
    return lotus::checker::EngineKind::Pulse;
  }
  if (engine == "kint") {
    return lotus::checker::EngineKind::KINT;
  }
  if (engine == "fitx") {
    return lotus::checker::EngineKind::FiTx;
  }
  if (engine == "concurrency") {
    return lotus::checker::EngineKind::Concurrency;
  }
  if (engine == "symex") {
    return lotus::checker::EngineKind::SymExec;
  }
  return std::nullopt;
}

std::vector<std::string> splitCsv(StringRef csv) {
  std::vector<std::string> values;
  SmallVector<StringRef, 8> pieces;
  csv.split(pieces, ',', -1, false);
  for (StringRef piece : pieces) {
    values.push_back(piece.trim().str());
  }
  return values;
}

} // namespace

int main(int argc, char **argv) {
  sys::PrintStackTraceOnErrorSignal(argv[0]);
  PrettyStackTraceProgram stack_trace(argc, argv);
  llvm_shutdown_obj shutdown;
  report_options::initializeReportOptions();

  cl::ParseCommandLineOptions(
      argc, argv,
      "Lotus generic checker driver\n"
      "  Run YAML-backed declarative checkers from share/checkers.\n");

  lotus::checker::CheckerRegistry registry;
  if (auto error = lotus::checker::registerBuiltinNativeCheckers(registry)) {
    logAllUnhandledErrors(std::move(error), errs(), "");
    return 1;
  }

  lotus::checker::CheckerSpecLoader loader;
  auto specs_or = loader.loadFromDirectory(BuiltinSpecDir);
  if (!specs_or) {
    logAllUnhandledErrors(specs_or.takeError(), errs(), "");
    return 1;
  }
  for (const auto &spec : *specs_or) {
    if (auto error = registry.registerDeclarative(spec)) {
      logAllUnhandledErrors(std::move(error), errs(), "");
      return 1;
    }
  }

  if (ListCheckers) {
    for (const auto *descriptor : registry.list()) {
      outs() << descriptor->metadata.id << "\t"
             << lotus::checker::toString(descriptor->metadata.engine) << "\t"
             << descriptor->metadata.category << "\t"
             << descriptor->metadata.title << "\n";
    }
    return 0;
  }

  if (InputFilename.empty()) {
    errs() << "error: input bitcode file is required unless --list-checkers is used\n";
    return 1;
  }

  LLVMContext context;
  SMDiagnostic error;
  std::unique_ptr<Module> module = parseIRFile(InputFilename, error, context);
  if (!module) {
    error.print(argv[0], errs());
    return 1;
  }

  std::vector<const lotus::checker::CheckerDescriptor *> selection;
  if (!CheckerIds.empty()) {
    for (const auto &id : splitCsv(CheckerIds)) {
      auto descriptor_or = registry.findById(id);
      if (!descriptor_or) {
        logAllUnhandledErrors(descriptor_or.takeError(), errs(), "");
        return 1;
      }
      selection.push_back(*descriptor_or);
    }
  } else {
    selection = registry.select(CategoryFilter, parseEngine(EngineFilter));
    std::vector<const lotus::checker::CheckerDescriptor *> defaults;
    for (const auto *descriptor : selection) {
      if (descriptor->metadata.default_enabled) {
        defaults.push_back(descriptor);
      }
    }
    selection = std::move(defaults);
  }

  if (selection.empty()) {
    errs() << "error: no checkers selected\n";
    return 1;
  }

  lotus::checker::CheckerContext checker_context{*module};
  lotus::checker::CheckerDriver driver(registry, checker_context);
  auto diagnostics_or = driver.run(selection);
  if (!diagnostics_or) {
    logAllUnhandledErrors(diagnostics_or.takeError(), errs(), "");
    return 1;
  }

  if (auto report_error = driver.emitToReportManager(*diagnostics_or)) {
    logAllUnhandledErrors(std::move(report_error), errs(), "");
    return 1;
  }

  BugReportMgr &mgr = BugReportMgr::get_instance();
  if (!report_options::SuppressionFile.empty()) {
    SuppressionManager supp_mgr;
    if (supp_mgr.loadFromFile(report_options::SuppressionFile)) {
      mgr.setSuppressionManager(&supp_mgr);
      mgr.filterSuppressed();
    }
  }

  mgr.deduplicate_reports(true);
  mgr.print_detailed_reports(outs(), VerboseReports,
                             report_options::MinConfidenceScore,
                             report_options::ShowInvalidReports);

  if (!report_options::JsonOutputFile.empty()) {
    std::error_code ec;
    raw_fd_ostream json_out(report_options::JsonOutputFile, ec, sys::fs::OF_None);
    if (ec) {
      errs() << "Error writing JSON report: " << ec.message() << "\n";
      return 1;
    }
    mgr.generate_json_report(json_out, report_options::MinConfidenceScore);
  }

  if (!report_options::SarifOutputFile.empty()) {
    std::error_code ec;
    raw_fd_ostream sarif_out(report_options::SarifOutputFile, ec,
                             sys::fs::OF_None);
    if (ec) {
      errs() << "Error writing SARIF report: " << ec.message() << "\n";
      return 1;
    }
    mgr.generate_sarif_report(sarif_out, report_options::MinConfidenceScore);
  }

  return 0;
}
