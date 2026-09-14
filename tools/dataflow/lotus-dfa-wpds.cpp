#include "llvm/Support/CommandLine.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/raw_ostream.h"

#include "Dataflow/WPDS/Analyses/ConstantPropagationAnalysis.h"
#include "Dataflow/WPDS/Analyses/LivenessAnalysis.h"
#include "Dataflow/WPDS/Analyses/TaintAnalysis.h"
#include "Dataflow/WPDS/Analyses/UninitializedVariablesAnalysis.h"
#include "Dataflow/WPDS/Backend.h"
#include "ToolSupport.h"

#include <memory>
#include <set>
#include <string>
#include <vector>

using namespace llvm;

static cl::opt<std::string> InputFilename(cl::Positional, cl::desc("<bitcode>"),
                                          cl::Required);
static cl::opt<std::string>
    AnalysisOpt("analysis",
                cl::desc("Analysis: liveness (default), constant_prop, taint, "
                         "uninitialized"),
                cl::init("liveness"));
static cl::opt<std::string> BackendOpt(
    "wpds-backend",
    cl::desc("WPDS backend: legacy (default), wali-fwpds, wali-swpds"),
    cl::init("legacy"));
static cl::opt<bool>
    StatisticsOpt("wpds-stats",
                  cl::desc("Print WPDS stage timings and model statistics"),
                  cl::init(false));
static cl::opt<bool> VerifyOpt(
    "wpds-verify-against-legacy",
    cl::desc("Compare every materialized observation with the legacy backend"),
    cl::init(false));
static cl::opt<unsigned> QueryCountOpt(
    "wpds-query-count",
    cl::desc("Number of queries to run on one prepared liveness model"),
    cl::init(1));

namespace {

std::unique_ptr<mono::DataFlowResult>
runAnalysis(Module &module, StringRef analysis,
            const wpds::WPDSBackendOptions &options,
            wpds::WPDSBackendStatistics &statistics, std::string &error) {
  if (analysis == "liveness") {
    return runLivenessAnalysis(module, options, &statistics, &error);
  }
  if (analysis == "constant_prop") {
    return runConstantPropagationAnalysis(module, options, &statistics, &error);
  }
  if (analysis == "taint") {
    return runTaintAnalysis(module, options, &statistics, &error);
  }
  if (analysis == "uninitialized") {
    return runUninitializedVariablesAnalysis(module, options, &statistics,
                                             &error);
  }
  error = "unknown WPDS analysis '" + analysis.str() + "'";
  return nullptr;
}

void printStatistics(raw_ostream &output,
                     const wpds::WPDSBackendStatistics &statistics) {
  output << "backend.selected=" << wpds::toString(statistics.selectedBackend)
         << "\nbackend.effective="
         << wpds::toString(statistics.effectiveBackend)
         << "\nquery.operation=" << wpds::toString(statistics.query)
         << "\nmodel.control_states=" << statistics.controlStateCount
         << "\nmodel.stack_symbols=" << statistics.stackSymbolCount
         << "\nmodel.rules.pop=" << statistics.popRuleCount
         << "\nmodel.rules.replace=" << statistics.replaceRuleCount
         << "\nmodel.rules.push=" << statistics.pushRuleCount
         << "\nsession.preparations=" << statistics.preparationCount
         << "\nsession.queries=" << statistics.queryCount
         << "\ntime.lowering_ms=" << statistics.loweringMilliseconds
         << "\ntime.preparation_ms=" << statistics.preparationMilliseconds
         << "\ntime.solving_ms=" << statistics.solvingMilliseconds
         << "\ntime.decoding_ms=" << statistics.decodingMilliseconds << "\n";
}

} // namespace

int main(int argc, char **argv) {
  InitLLVM initialization(argc, argv);
  cl::ParseCommandLineOptions(argc, argv, "Lotus WPDS dataflow analysis\n");

  std::optional<wpds::WPDSBackendKind> backend =
      wpds::parseWPDSBackend(BackendOpt);
  if (!backend.has_value()) {
    errs() << "error: unknown WPDS backend '" << BackendOpt
           << "'; expected legacy, wali-fwpds, or wali-swpds\n";
    return 1;
  }
  if (!wpds::isWPDSBackendAvailable(*backend)) {
    errs() << "error: " << wpds::getWPDSBackendUnavailableReason(*backend)
           << "\n";
    return 1;
  }

  LLVMContext context;
  SMDiagnostic diagnostic;
  auto module = lotus::dataflow_tool::loadModuleOrReport(InputFilename, context,
                                                         diagnostic, argv[0]);
  if (!module) {
    return 1;
  }
  lotus::dataflow_tool::prepareModule(*module);

  wpds::WPDSBackendOptions options;
  options.backend = *backend;
  options.verifyAgainstLegacy = VerifyOpt;
  options.collectStatistics = StatisticsOpt;
  wpds::WPDSBackendStatistics statistics;
  std::string error;
  std::unique_ptr<mono::DataFlowResult> result = nullptr;
  if (QueryCountOpt == 0) {
    errs() << "error: --wpds-query-count must be at least one\n";
    return 1;
  }
  if (QueryCountOpt > 1) {
    if (AnalysisOpt != "liveness") {
      errs() << "error: --wpds-query-count currently requires "
                "--analysis=liveness\n";
      return 1;
    }
    auto prepared = prepareLivenessAnalysis(*module, options, &error);
    if (prepared) {
      std::vector<Value *> seedValues;
      for (GlobalVariable &global : module->globals()) {
        seedValues.push_back(&global);
      }
      for (Function &function : *module) {
        if (function.isDeclaration()) {
          continue;
        }
        for (Argument &argument : function.args()) {
          seedValues.push_back(&argument);
        }
        for (BasicBlock &block : function) {
          for (Instruction &instruction : block) {
            seedValues.push_back(&instruction);
          }
        }
      }
      if (QueryCountOpt > seedValues.size() + 1) {
        errs() << "error: input provides only " << seedValues.size() + 1
               << " distinct liveness boundary seeds\n";
        return 1;
      }
      for (unsigned query = 0; query < QueryCountOpt; ++query) {
        std::set<Value *> seed;
        if (query != 0) {
          seed.insert(seedValues[query - 1]);
        }
        result = prepared->solve(seed);
        if (!result) {
          error = prepared->getLastError();
          break;
        }
      }
      statistics = prepared->getStatistics();
    }
  } else {
    result = runAnalysis(*module, AnalysisOpt, options, statistics, error);
  }
  if (!result) {
    errs() << "error: " << (error.empty() ? "WPDS analysis failed" : error)
           << "\n";
    return 1;
  }

  outs() << "[wpds:" << AnalysisOpt
         << " backend=" << wpds::toString(statistics.effectiveBackend) << "]\n";
  lotus::dataflow_tool::forEachDefinedFunction(
      *module, outs(), [&](const lotus::dataflow_tool::FunctionView &view) {
        for (Instruction *instruction : view.OrderedInsts) {
          outs() << "  " << view.ValueToId.at(instruction) << " IN: ";
          lotus::dataflow_tool::formatValueSet(outs(), result->IN(instruction),
                                               view.ValueToId);
          outs() << " OUT: ";
          lotus::dataflow_tool::formatValueSet(outs(), result->OUT(instruction),
                                               view.ValueToId);
          outs() << "\n";
        }
      });
  if (StatisticsOpt) {
    printStatistics(outs(), statistics);
  }
  return 0;
}
