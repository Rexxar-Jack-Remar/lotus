#include "Checker/Core/CheckerDriver.h"
#include "Checker/Core/CheckerRegistry.h"
#include "Checker/Core/CheckerSpecLoader.h"
#include "Checker/Report/BugReportMgr.h"
#include "Checker/Report/ReportOptions.h"
#include "Checker/Report/SuppressionManager.h"
#include "Checker/Tooling/CheckerSubcommands.h"
#include "CheckerReport.h"
#include "CheckerToolEntrypoints.h"

#include <optional>
#include <set>
#include <string>
#include <vector>

#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/InitLLVM.h>
#include <llvm/Support/Path.h>
#include <llvm/Support/PrettyStackTrace.h>
#include <llvm/Support/Process.h>
#include <llvm/Support/Signals.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;

static cl::OptionCategory GenericSelectionCategory(
    "Generic Checker Selection Options",
    "Options for selecting registry-backed declarative checkers");
static cl::OptionCategory GenericExecutionCategory(
    "Generic Checker Execution Options",
    "Options for running the generic declarative checker driver");
static cl::opt<std::string>
    InputFilename(cl::Positional, cl::desc("<input bitcode file>"),
                  cl::value_desc("bitcode"), cl::init(""),
                  cl::cat(GenericExecutionCategory),
                  cl::sub(*cl::TopLevelSubCommand),
                  cl::sub(lotus::checker::tooling::genericSubCommand()));
static cl::opt<bool>
    ListCheckers("list-checkers",
                 cl::desc("List available checker ids and exit"),
                 cl::cat(GenericSelectionCategory), cl::init(false),
                 cl::sub(*cl::TopLevelSubCommand),
                 cl::sub(lotus::checker::tooling::genericSubCommand()));
static cl::opt<std::string> CheckerIds(
    "checker", cl::desc("Run only the given comma-separated checker ids"),
    cl::value_desc("id[,id...]"), cl::init(""),
    cl::cat(GenericSelectionCategory), cl::sub(*cl::TopLevelSubCommand),
    cl::sub(lotus::checker::tooling::genericSubCommand()));
static cl::opt<std::string> CategoryFilter(
    "category", cl::desc("Run only checkers in the given category"),
    cl::value_desc("category"), cl::init(""), cl::cat(GenericSelectionCategory),
    cl::sub(*cl::TopLevelSubCommand),
    cl::sub(lotus::checker::tooling::genericSubCommand()));
static cl::opt<std::string> BuiltinSpecDir(
    "spec-dir", cl::desc("Load declarative checker specs from this directory"),
    cl::value_desc("dir"), cl::init(""), cl::cat(GenericExecutionCategory),
    cl::sub(*cl::TopLevelSubCommand),
    cl::sub(lotus::checker::tooling::genericSubCommand()));
static cl::opt<bool>
    VerboseReports("v",
                   cl::desc("Include trace and IR details in printed reports"),
                   cl::init(false), cl::cat(GenericExecutionCategory),
                   cl::sub(*cl::TopLevelSubCommand),
                   cl::sub(lotus::checker::tooling::genericSubCommand()));

namespace {

bool isHelpFlag(StringRef arg) {
  return arg == "-h" || arg == "--help" || arg == "--help-hidden" ||
         arg == "--help-list" || arg == "--help-list-hidden";
}

bool isKnownCheckerSubcommand(StringRef arg) {
  return arg == "generic" || arg == "kint" || arg == "taint" ||
         arg == "concur" || arg == "pulse" || arg == "fitx" || arg == "saber" ||
         arg == "ae" || arg == "symex";
}

std::optional<std::string> requestedSubcommand(int argc, char **argv) {
  for (int i = 1; i < argc; ++i) {
    StringRef arg(argv[i]);
    if (!arg.empty() && arg[0] != '-') {
      if (isKnownCheckerSubcommand(arg)) {
        return arg.str();
      }
      return std::nullopt;
    }
  }
  return std::nullopt;
}

bool helpRequested(int argc, char **argv) {
  for (int i = 1; i < argc; ++i) {
    if (isHelpFlag(argv[i])) {
      return true;
    }
  }
  return false;
}

bool hiddenHelpRequested(int argc, char **argv) {
  for (int index = 1; index < argc; ++index) {
    StringRef argument(argv[index]);
    if (argument == "--help-hidden" || argument == "--help-list-hidden") {
      return true;
    }
  }
  return false;
}

cl::SubCommand *checkerSubcommand(StringRef engine) {
  if (engine == "generic")
    return &lotus::checker::tooling::genericSubCommand();
  if (engine == "kint")
    return &lotus::checker::tooling::kintSubCommand();
  if (engine == "taint")
    return &lotus::checker::tooling::taintSubCommand();
  if (engine == "concur")
    return &lotus::checker::tooling::concurrencySubCommand();
  if (engine == "pulse")
    return &lotus::checker::tooling::pulseSubCommand();
  if (engine == "fitx")
    return &lotus::checker::tooling::fitxSubCommand();
  if (engine == "saber")
    return &lotus::checker::tooling::saberSubCommand();
  if (engine == "ae")
    return &lotus::checker::tooling::aeSubCommand();
  if (engine == "symex")
    return &lotus::checker::tooling::symexSubCommand();
  return nullptr;
}

void printTopLevelHelp() {
  outs() << "OVERVIEW: Lotus checker front-end\n\n"
         << "USAGE:\n"
         << "  lotus-check --engine=<name> [engine options] <input bitcode>\n"
         << "  lotus-check --list-checkers\n\n"
         << "ENGINES:\n"
         << "  generic  Registry-backed declarative checkers\n"
         << "  ae       Abstract execution\n"
         << "  kint     Integer bug detection\n"
         << "  taint    IFDS taint analysis\n"
         << "  concur   Concurrency checking\n"
         << "  pulse    Pulse memory-safety analysis\n"
         << "  fitx     FiTx typestate analysis\n"
         << "  saber    Sparse value-flow checking\n"
         << "  symex    Symbolic execution\n\n"
         << "Use --engine=<name> --help for engine-specific options.\n";
}

void printEngineHelp(StringRef engine, cl::SubCommand &subcommand,
                     bool showHidden) {
  outs() << "OVERVIEW: Lotus " << engine << " engine\n\n"
         << "USAGE: lotus-check --engine=" << engine
         << " [options] <input bitcode file>\n\n"
         << "OPTIONS:\n\n";

  std::set<cl::Option *> uniqueOptions;
  for (const auto &entry : subcommand.OptionsMap) {
    cl::Option *option = entry.second;
    if (!option || option->isPositional() ||
        (!showHidden && option->getOptionHiddenFlag() != cl::NotHidden)) {
      continue;
    }
    uniqueOptions.insert(option);
  }

  std::vector<cl::Option *> options(uniqueOptions.begin(), uniqueOptions.end());
  llvm::sort(options, [](const cl::Option *left, const cl::Option *right) {
    return left->ArgStr < right->ArgStr;
  });
  size_t width = 0;
  for (const cl::Option *option : options) {
    width = std::max(width, option->getOptionWidth());
  }
  for (const cl::Option *option : options) {
    option->printOptionInfo(width);
  }
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

StringRef cliEngineName(lotus::checker::EngineKind engine) {
  switch (engine) {
  case lotus::checker::EngineKind::Declarative:
    return "generic";
  case lotus::checker::EngineKind::AE:
    return "ae";
  case lotus::checker::EngineKind::Saber:
    return "saber";
  case lotus::checker::EngineKind::Pulse:
    return "pulse";
  case lotus::checker::EngineKind::KINT:
    return "kint";
  case lotus::checker::EngineKind::FiTx:
    return "fitx";
  case lotus::checker::EngineKind::Concurrency:
    return "concur";
  case lotus::checker::EngineKind::SymExec:
    return "symex";
  }
  llvm_unreachable("unhandled checker engine");
}

Expected<std::vector<std::string>> normalizeEngineSelectionArgs(int argc,
                                                                char **argv) {
  if (argc > 1 && isKnownCheckerSubcommand(argv[1])) {
    return createStringError(
        inconvertibleErrorCode(),
        "engine subcommands are not part of the public CLI; use --engine=%s",
        argv[1]);
  }

  std::optional<std::string> selectedEngine;
  std::vector<std::string> normalized;
  normalized.reserve(argc + 1);
  normalized.push_back(argv[0]);

  for (int index = 1; index < argc; ++index) {
    StringRef argument(argv[index]);
    StringRef engineName;
    if (argument.consume_front("--engine=")) {
      engineName = argument;
    } else if (argument == "--engine") {
      if (++index >= argc) {
        return createStringError(inconvertibleErrorCode(),
                                 "--engine requires a value");
      }
      engineName = argv[index];
    } else {
      normalized.push_back(argv[index]);
      continue;
    }

    if (selectedEngine) {
      return createStringError(inconvertibleErrorCode(),
                               "--engine may only be specified once");
    }
    if (!isKnownCheckerSubcommand(engineName)) {
      return createStringError(
          inconvertibleErrorCode(),
          "invalid engine '%s'; expected generic, ae, kint, taint, concur, "
          "pulse, fitx, saber, or symex",
          engineName.str().c_str());
    }
    selectedEngine = engineName.str();
  }

  if (selectedEngine) {
    normalized.insert(normalized.begin() + 1, *selectedEngine);
  }
  return normalized;
}

Expected<lotus::checker::CheckerRegistry>
buildRegistry(bool tolerateSpecLoadFailure) {
  lotus::checker::CheckerRegistry registry;
  if (auto error = lotus::checker::registerBuiltinNativeCheckers(registry)) {
    return std::move(error);
  }

  std::string specDir;
  if (BuiltinSpecDir.getNumOccurrences() != 0) {
    specDir = BuiltinSpecDir;
  } else if (auto environment =
                 sys::Process::GetEnv("LOTUS_CHECKER_SPEC_DIR")) {
    specDir = *environment;
  } else if (sys::fs::is_directory(LOTUS_INSTALL_CHECKER_SPEC_DIR)) {
    specDir = LOTUS_INSTALL_CHECKER_SPEC_DIR;
  } else {
    specDir = LOTUS_SOURCE_CHECKER_SPEC_DIR;
  }

  lotus::checker::CheckerSpecLoader loader;
  auto specs_or = loader.loadFromDirectory(specDir);
  if (!specs_or) {
    if (tolerateSpecLoadFailure) {
      logAllUnhandledErrors(
          specs_or.takeError(), errs(),
          "warning: could not load declarative checker specs: ");
      return registry;
    }
    return specs_or.takeError();
  }
  for (const auto &spec : *specs_or) {
    if (auto error = registry.registerDeclarative(spec)) {
      return std::move(error);
    }
  }

  return registry;
}

} // namespace

int runGenericCheckerTool(const char *argv0) {
  auto registry_or = buildRegistry(ListCheckers);
  if (!registry_or) {
    logAllUnhandledErrors(registry_or.takeError(), errs(), "");
    return lotus::checker::tooling::EXIT_ERROR;
  }
  lotus::checker::CheckerRegistry registry = std::move(*registry_or);

  if (ListCheckers) {
    outs() << "ID\tENGINE\tMODE\tCATEGORY\tTITLE\n";
    for (const auto *descriptor : registry.list()) {
      outs() << descriptor->metadata.id << "\t"
             << cliEngineName(descriptor->metadata.engine) << "\t"
             << (descriptor->isDeclarative() ? "generic" : "native-engine")
             << "\t" << descriptor->metadata.category << "\t"
             << descriptor->metadata.title << "\n";
    }
    return lotus::checker::tooling::EXIT_SUCCESS_CODE;
  }

  if (InputFilename.empty()) {
    errs() << "error: input bitcode file is required unless --list-checkers is "
              "used\n";
    return lotus::checker::tooling::EXIT_ERROR;
  }

  LLVMContext context;
  SMDiagnostic error;
  std::unique_ptr<Module> module = parseIRFile(InputFilename, error, context);
  if (!module) {
    error.print(argv0, errs());
    return lotus::checker::tooling::EXIT_ERROR;
  }

  std::vector<const lotus::checker::CheckerDescriptor *> selection;
  if (!CheckerIds.empty()) {
    for (const auto &id : splitCsv(CheckerIds)) {
      auto descriptor_or = registry.findById(id);
      if (!descriptor_or) {
        logAllUnhandledErrors(descriptor_or.takeError(), errs(), "");
        return lotus::checker::tooling::EXIT_ERROR;
      }
      const auto *descriptor = *descriptor_or;
      if (!CategoryFilter.empty() &&
          descriptor->metadata.category != CategoryFilter) {
        continue;
      }
      selection.push_back(descriptor);
    }
  } else {
    selection = registry.select(CategoryFilter,
                                lotus::checker::EngineKind::Declarative);
    if (CategoryFilter.empty()) {
      std::vector<const lotus::checker::CheckerDescriptor *> defaults;
      for (const auto *descriptor : selection) {
        if (descriptor->metadata.default_enabled) {
          defaults.push_back(descriptor);
        }
      }
      selection = std::move(defaults);
    }
  }

  if (selection.empty()) {
    errs() << "error: no checkers selected\n";
    return lotus::checker::tooling::EXIT_ERROR;
  }

  for (const auto *descriptor : selection) {
    if (descriptor != nullptr && !descriptor->isDeclarative()) {
      errs() << "error: checker '" << descriptor->metadata.id
             << "' uses the native "
             << cliEngineName(descriptor->metadata.engine)
             << " engine and cannot run in generic mode\n"
             << "hint: select it with --engine=<name> as shown by "
                "--list-checkers\n";
      return lotus::checker::tooling::EXIT_ERROR;
    }
  }

  lotus::checker::CheckerContext checker_context{*module};
  lotus::checker::CheckerDriver driver(registry, checker_context);
  auto diagnostics_or = driver.run(selection);
  if (!diagnostics_or) {
    logAllUnhandledErrors(diagnostics_or.takeError(), errs(), "");
    return lotus::checker::tooling::EXIT_ERROR;
  }

  if (auto report_error = driver.emitToReportManager(*diagnostics_or)) {
    logAllUnhandledErrors(std::move(report_error), errs(), "");
    return lotus::checker::tooling::EXIT_ERROR;
  }

  BugReportMgr &mgr = BugReportMgr::get_instance();
  return lotus::checker::tooling::emitCheckerReports(mgr, {VerboseReports});
}

int main(int argc, char **argv) {
  auto normalizedArgsOr = normalizeEngineSelectionArgs(argc, argv);
  if (!normalizedArgsOr) {
    logAllUnhandledErrors(normalizedArgsOr.takeError(), errs(), "error: ");
    return lotus::checker::tooling::EXIT_ERROR;
  }
  std::vector<std::string> normalizedArgs = std::move(*normalizedArgsOr);
  std::vector<char *> normalizedArgv;
  normalizedArgv.reserve(normalizedArgs.size() + 1);
  for (std::string &argument : normalizedArgs) {
    normalizedArgv.push_back(argument.data());
  }
  normalizedArgv.push_back(nullptr);
  int normalizedArgc = static_cast<int>(normalizedArgs.size());
  char **argumentVector = normalizedArgv.data();

  sys::PrintStackTraceOnErrorSignal(argumentVector[0]);
  PrettyStackTraceProgram stack_trace(normalizedArgc, argumentVector);
  llvm::InitLLVM init_llvm(normalizedArgc, argumentVector);
  llvm_shutdown_obj shutdown;
  report_options::initializeReportOptions();

  const bool wants_help = helpRequested(normalizedArgc, argumentVector);
  const auto requested_subcommand =
      requestedSubcommand(normalizedArgc, argumentVector);
  if (wants_help) {
    if (!requested_subcommand) {
      printTopLevelHelp();
      return lotus::checker::tooling::EXIT_SUCCESS_CODE;
    }
    cl::SubCommand *subcommand = checkerSubcommand(*requested_subcommand);
    assert(subcommand && "normalized engine must have a subcommand");
    const bool showHidden = hiddenHelpRequested(normalizedArgc, argumentVector);
    printEngineHelp(*requested_subcommand, *subcommand, showHidden);
    return lotus::checker::tooling::EXIT_SUCCESS_CODE;
  }

  cl::ParseCommandLineOptions(
      normalizedArgc, argumentVector,
      "Lotus checker front-end\n"
      "  Select one engine with --engine=<name>.\n"
      "  Example: 'lotus-check --engine=symex --help' shows Symbolic "
      "Execution options.\n");

  if (lotus::checker::tooling::kintSubCommand()) {
    return runKintCheckerTool(argumentVector[0]);
  }
  if (lotus::checker::tooling::taintSubCommand()) {
    return runTaintCheckerTool(argumentVector[0]);
  }
  if (lotus::checker::tooling::concurrencySubCommand()) {
    return runConcurrencyCheckerTool(argumentVector[0]);
  }
  if (lotus::checker::tooling::pulseSubCommand()) {
    return runPulseCheckerTool(argumentVector[0]);
  }
  if (lotus::checker::tooling::fitxSubCommand()) {
    return runFiTxCheckerTool(argumentVector[0]);
  }
  if (lotus::checker::tooling::saberSubCommand()) {
    return runSaberCheckerTool(argumentVector[0]);
  }
  if (lotus::checker::tooling::aeSubCommand()) {
    return runAECheckerTool(argumentVector[0]);
  }
  if (lotus::checker::tooling::symexSubCommand()) {
    return runSymExCheckerTool(argumentVector[0]);
  }
  if (lotus::checker::tooling::genericSubCommand() || ListCheckers) {
    return runGenericCheckerTool(argumentVector[0]);
  }

  errs() << "error: no engine selected\n";
  errs() << "hint: use --engine=<name> or --list-checkers\n";
  return lotus::checker::tooling::EXIT_ERROR;
}
