/**
 * @file llvm-ai.cpp
 * @brief LLVM IFDS/IDE Analysis Tool
 *
 * A command-line tool for running IFDS/IDE interprocedural dataflow analysis
 */

#include "Utils/LLVM/Demangle.h"
#include "Checker/Tooling/CheckerSubcommands.h"
#include "CheckerReport.h"

#include <chrono>
#include <memory>
#include <set>
#include <sstream>
#include <string>

#include <llvm/ADT/Statistic.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/ErrorOr.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/Path.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>
#include <Alias/Infrastructure/AliasAnalysisWrapper/AliasAnalysisWrapper.h>
#include <Dataflow/IFDS/Analyses/IFDSTaintAnalysis.h>
#include <Dataflow/IFDS/Core/IFDSFramework.h>
#include <Dataflow/IFDS/Solver/IFDSSolver.h>

// #include <iostream>
// #include <thread>

using namespace llvm;
using namespace ifds;

static cl::opt<std::string> InputFilename(cl::Positional,
                                          cl::desc("<input bitcode file>"),
                                          cl::Required,
                                          cl::sub(lotus::checker::tooling::taintSubCommand()));

static cl::opt<bool> Verbose("verbose", cl::desc("Enable verbose output"),
                             cl::init(false),
                             cl::sub(lotus::checker::tooling::taintSubCommand()));

static cl::opt<int> AnalysisType("analysis",
                                 cl::desc("Type of analysis to run: 0=taint"),
                                 cl::init(0),
                                 cl::sub(lotus::checker::tooling::taintSubCommand()));

static cl::opt<std::string> AliasAnalysisType(
    "aa",
    cl::desc(
        "Alias analysis type: andersen, dyck, cfl-anders, cfl-steens, seadsa, "
        "allocaa, basic, combined=Andersen(NoCtx)+DyckAA (default: dyck)"),
    cl::init("dyck"),
    cl::sub(lotus::checker::tooling::taintSubCommand()));

static cl::opt<bool> ShowResults("show-results",
                                 cl::desc("Show detailed analysis results"),
                                 cl::init(true),
                                 cl::sub(lotus::checker::tooling::taintSubCommand()));

static cl::opt<int>
    MaxDetailedResults("max-results",
                       cl::desc("Maximum number of detailed results to show"),
                       cl::init(10),
                       cl::sub(lotus::checker::tooling::taintSubCommand()));

static cl::opt<std::string>
    SourceFunctions("sources",
                    cl::desc("Comma-separated list of source functions"),
                    cl::init(""),
                    cl::sub(lotus::checker::tooling::taintSubCommand()));

static cl::opt<std::string>
    SinkFunctions("sinks", cl::desc("Comma-separated list of sink functions"),
                  cl::init(""),
                  cl::sub(lotus::checker::tooling::taintSubCommand()));

static cl::opt<bool>
    MicroBench("micro-bench",
               cl::desc("Enable micro benchmark mode (use source/sink and "
                        "evaluate precision/recall)"),
               cl::init(false),
               cl::sub(lotus::checker::tooling::taintSubCommand()));

static cl::opt<std::string> ExpectedFile(
    "expected",
    cl::desc("Path to .expected file for micro benchmark evaluation"),
    cl::init(""),
    cl::sub(lotus::checker::tooling::taintSubCommand()));

static cl::opt<bool> PrintStats("print-stats",
                                cl::desc("Print LLVM statistics"),
                                cl::init(false),
                                cl::sub(lotus::checker::tooling::taintSubCommand()));

// Helper function to parse comma-separated function names
std::vector<std::string> parseFunctionList(const std::string &input) {
  std::vector<std::string> functions;
  if (input.empty())
    return functions;

  std::stringstream ss(input);
  std::string item;
  while (std::getline(ss, item, ',')) {
    StringRef trimmed = StringRef(item).trim();
    if (!trimmed.empty()) {
      functions.push_back(trimmed.str());
    }
  }
  return functions;
}

using TaintFlow = std::pair<std::string, std::string>;

static ErrorOr<std::set<TaintFlow>> loadExpectedFlows(StringRef filename) {
  auto bufferOr = MemoryBuffer::getFile(filename);
  if (!bufferOr) {
    return bufferOr.getError();
  }

  std::set<TaintFlow> flows;
  SmallVector<StringRef, 32> lines;
  bufferOr.get()->getBuffer().split(lines, '\n');
  for (size_t index = 0; index < lines.size(); ++index) {
    StringRef line = lines[index].split('#').first.trim();
    if (line.empty()) {
      continue;
    }

    StringRef source;
    StringRef sink;
    size_t arrow = line.find("->");
    if (arrow != StringRef::npos) {
      source = line.take_front(arrow).trim();
      sink = line.drop_front(arrow + 2).trim();
    } else {
      auto parts = line.split(',');
      source = parts.first.trim();
      sink = parts.second.trim();
    }
    if (source.empty() || sink.empty() || sink.contains(',')) {
      errs() << "error: invalid expected flow at " << filename << ":"
             << (index + 1) << " (expected source->sink)\n";
      return std::make_error_code(std::errc::invalid_argument);
    }
    flows.emplace(source.str(), sink.str());
  }
  return flows;
}

static std::set<TaintFlow>
collectDetectedFlows(const TaintAnalysis &analysis,
                     const IFDSSolver<TaintAnalysis> &solver) {
  std::set<TaintFlow> flows;
  for (const auto &entry : solver.get_all_results()) {
    const auto &node = entry.first;
    const auto &facts = entry.second;
    const auto *sinkCall = dyn_cast_or_null<CallBase>(node.instruction);
    if (!sinkCall || !analysis.is_sink(sinkCall) ||
        !sinkCall->getCalledFunction()) {
      continue;
    }

    for (const TaintFact &fact : facts) {
      bool reachesArgument = false;
      for (const Use &argument : sinkCall->args()) {
        if (analysis.is_argument_tainted(argument.get(), fact)) {
          reachesArgument = true;
          break;
        }
      }
      if (!reachesArgument) {
        continue;
      }

      const auto *directSource =
          dyn_cast_or_null<CallBase>(fact.get_source());
      if (directSource && directSource->getCalledFunction()) {
        flows.emplace(directSource->getCalledFunction()->getName().str(),
                      sinkCall->getCalledFunction()->getName().str());
        continue;
      }

      auto path = analysis.trace_taint_sources_summary_based(
          solver, sinkCall, fact);
      for (const Instruction *sourceInst : path.sources) {
        const auto *sourceCall = dyn_cast_or_null<CallBase>(sourceInst);
        if (sourceCall && sourceCall->getCalledFunction()) {
          flows.emplace(sourceCall->getCalledFunction()->getName().str(),
                        sinkCall->getCalledFunction()->getName().str());
        }
      }
    }
  }
  return flows;
}

static void evaluateMicroBenchmark(const std::set<TaintFlow> &expected,
                                   const std::set<TaintFlow> &detected,
                                   raw_ostream &OS) {
  size_t truePositives = 0;
  for (const TaintFlow &flow : detected) {
    truePositives += expected.count(flow);
  }
  const size_t falsePositives = detected.size() - truePositives;
  const size_t falseNegatives = expected.size() - truePositives;
  const double precision = detected.empty()
                               ? (expected.empty() ? 1.0 : 0.0)
                               : static_cast<double>(truePositives) /
                                     detected.size();
  const double recall = expected.empty()
                            ? 1.0
                            : static_cast<double>(truePositives) /
                                  expected.size();
  const double f1 = precision + recall == 0.0
                        ? 0.0
                        : 2.0 * precision * recall / (precision + recall);

  OS << "\nMicro-benchmark evaluation:\n"
     << "  TP: " << truePositives << "  FP: " << falsePositives
     << "  FN: " << falseNegatives << "\n"
     << "  Precision: " << precision << "\n"
     << "  Recall: " << recall << "\n"
     << "  F1: " << f1 << "\n";
}

static void dumpSourceSinkMatches(const llvm::Module &module,
                                  const TaintAnalysis &analysis,
                                  llvm::raw_ostream &OS) {
  size_t total_calls = 0;
  size_t source_calls = 0;
  size_t sink_calls = 0;

  auto demangle_name = [](const std::string &name) {
    return DemangleUtils::demangle(name);
  };

  OS << "\nDetected call sites (source/sink tagging):\n";
  OS << "=========================================\n";

  for (const auto &function : module) {
    for (const auto &inst : instructions(function)) {
      const auto *call = llvm::dyn_cast<llvm::CallInst>(&inst);
      if (!call || !call->getCalledFunction())
        continue;

      ++total_calls;
      bool is_source = analysis.is_source(call);
      bool is_sink = analysis.is_sink(call);
      if (is_source)
        ++source_calls;
      if (is_sink)
        ++sink_calls;

      auto raw_name = call->getCalledFunction()->getName().str();
      auto demangled_name = demangle_name(raw_name);
      const DebugLoc debugLocation = call->getDebugLoc();
      const unsigned line = debugLocation ? debugLocation.getLine() : 0;

      OS << "  ";
      if (is_source)
        OS << "[source] ";
      if (is_sink)
        OS << "[sink] ";
      if (!is_source && !is_sink)
        OS << "[ ] ";
      OS << raw_name;
      if (demangled_name != raw_name) {
        OS << " -> " << demangled_name;
      }
      if (line > 0) {
        OS << " @ line " << line;
      }
      OS << "\n";
    }
  }

  OS << "Summary: " << total_calls << " calls, " << source_calls << " sources, "
     << sink_calls << " sinks\n";
}

// Helper function to parse alias analysis configuration from string
lotus::AAConfig parseAliasAnalysisConfig(const std::string &aaTypeStr) {
  return lotus::parseAAConfigFromString(aaTypeStr, lotus::AAConfig::DyckAA());
}

int runTaintCheckerTool(const char *argv0) {
  // Enable statistics collection if requested
  if (PrintStats) {
    llvm::EnableStatistics();
  }

  // Set up LLVM context and source manager
  LLVMContext Context;
  SMDiagnostic Err;

  // Load the input module
  std::unique_ptr<Module> M = parseIRFile(InputFilename, Err, Context);
  if (!M) {
    Err.print(argv0, errs());
    return lotus::checker::tooling::EXIT_ERROR;
  }

  if (Verbose) {
    outs() << "Loaded module: " << M->getName() << "\n";
    outs() << "Functions in module: " << M->size() << "\n";
  }

  // Set up alias analysis wrapper
  lotus::AAConfig aaConfig =
      parseAliasAnalysisConfig(AliasAnalysisType.getValue());
  auto aliasWrapper =
      std::make_unique<lotus::AliasAnalysisWrapper>(*M, aaConfig);

  if (Verbose) {
    outs() << "Using alias analysis: "
           << lotus::AliasAnalysisFactory::getTypeName(aaConfig) << "\n";
  }

  if (!aliasWrapper->isInitialized()) {
    errs() << "Warning: Alias analysis failed to initialize properly\n";
  }

  // Run the selected analysis
  try {
    switch (AnalysisType.getValue()) {
    case 0: { // Taint analysis
      outs() << "Running interprocedural taint analysis...\n";

      TaintAnalysis taintAnalysis;

      // Set up custom sources and sinks if provided
      auto sources = parseFunctionList(SourceFunctions);
      auto sinks = parseFunctionList(SinkFunctions);

      if (MicroBench) {
        sources.push_back("source");
        sinks.push_back("sink");
      }

      for (const auto &source : sources) {
        taintAnalysis.add_source_function(source);
      }
      for (const auto &sink : sinks) {
        taintAnalysis.add_sink_function(sink);
      }

      // Set up alias analysis
      taintAnalysis.set_alias_analysis(aliasWrapper.get());

      if (Verbose) {
        dumpSourceSinkMatches(*M, taintAnalysis, outs());
      }

      auto analysisStart = std::chrono::high_resolution_clock::now();

      outs() << "Using sequential IFDS solver\n";

      ifds::IFDSSolver<ifds::TaintAnalysis> solver(taintAnalysis);

      // Enable progress bar when running in verbose mode
      if (Verbose) {
        auto config = solver.get_solver_config();
        config.set_enable_progress_reporting(true);
        solver.set_solver_config(config);
      }

      solver.solve(*M);

      auto analysisEnd = std::chrono::high_resolution_clock::now();
      auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
          analysisEnd - analysisStart);

      outs() << "Sequential analysis completed in " << duration.count()
             << " ms\n";

      if (ShowResults) {
        taintAnalysis.report_vulnerabilities(solver, outs(),
                                             MaxDetailedResults.getValue());
      }
      if (MicroBench) {
        SmallString<256> expectedPath;
        if (!ExpectedFile.empty()) {
          expectedPath = ExpectedFile;
        } else {
          expectedPath = InputFilename;
          sys::path::replace_extension(expectedPath, "expected");
        }
        auto expectedOr = loadExpectedFlows(expectedPath);
        if (!expectedOr) {
          errs() << "error: could not read expected flows from "
                 << expectedPath << ": " << expectedOr.getError().message()
                 << "\n";
          return lotus::checker::tooling::EXIT_ERROR;
        }
        evaluateMicroBenchmark(*expectedOr,
                               collectDetectedFlows(taintAnalysis, solver),
                               outs());
      }
      break;
    }
    default:
      errs() << "Unknown analysis type\n";
      return lotus::checker::tooling::EXIT_ERROR;
    }

    outs() << "Analysis completed successfully.\n";

  } catch (const std::exception &e) {
    errs() << "Error running analysis: " << e.what() << "\n";
    return lotus::checker::tooling::EXIT_ERROR;
  }

  // Statistics will be printed automatically at program exit if enabled

  return lotus::checker::tooling::EXIT_SUCCESS_CODE;
}
