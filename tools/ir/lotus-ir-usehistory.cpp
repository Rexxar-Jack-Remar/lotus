#include "IR/ICFG/ICFGBuilder.h"
#include "IR/SVFG/SVFGBuilder.h"
#include "IR/UseHistory/DefectDetector.h"
#include "IR/UseHistory/LotusSVFG.h"
#include "IR/UseHistory/Query.h"

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/InitLLVM.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>

#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>

using namespace lotus::analysis;
using namespace lotus::usehistory;

namespace {
llvm::cl::opt<std::string> Input(llvm::cl::Positional,
                                  llvm::cl::desc("<input.ll or input.bc>"),
                                  llvm::cl::Required);
llvm::cl::opt<std::string> Format("format", llvm::cl::desc("text, json, or dot"),
                                   llvm::cl::init("text"));
llvm::cl::opt<std::string> DumpSVFG("dump-svfg",
                                    llvm::cl::desc("Write the source SVFG DOT to a file"),
                                    llvm::cl::init(""));
llvm::cl::opt<std::string> Check("check",
                                 llvm::cl::desc("double-free or use-after-free"),
                                 llvm::cl::init(""));
llvm::cl::opt<unsigned> Source("source-node",
                                llvm::cl::desc("SVFG source node ID for a flow query"),
                                llvm::cl::init(std::numeric_limits<unsigned>::max()));
llvm::cl::opt<unsigned> Sink("sink-node",
                              llvm::cl::desc("SVFG sink node ID for a flow query"),
                              llvm::cl::init(std::numeric_limits<unsigned>::max()));

const char *statusName(QueryStatus status) {
  switch (status) {
  case QueryStatus::Found: return "Found";
  case QueryStatus::NotFound: return "NotFound";
  case QueryStatus::Unknown: return "Unknown";
  }
  return "Unknown";
}
} // namespace

int main(int argc, char **argv) {
  llvm::InitLLVM init(argc, argv);
  llvm::cl::ParseCommandLineOptions(argc, argv, "Lotus SVFG use histories\n");
  bool query = Source != std::numeric_limits<unsigned>::max() ||
               Sink != std::numeric_limits<unsigned>::max();
  if ((Source == std::numeric_limits<unsigned>::max()) !=
      (Sink == std::numeric_limits<unsigned>::max())) {
    llvm::errs() << "lotus-ir-usehistory: supply both --source-node and --sink-node\n";
    return 1;
  }
  if (Format != "text" && Format != "json" && Format != "dot") {
    llvm::errs() << "lotus-ir-usehistory: --format must be text, json, or dot\n";
    return 1;
  }
  if (query && Format != "text") {
    llvm::errs() << "lotus-ir-usehistory: queries require --format=text\n";
    return 1;
  }
  if (!Check.empty() && query) {
    llvm::errs() << "lotus-ir-usehistory: --check cannot be combined with a node query\n";
    return 1;
  }
  if (!Check.empty() && Format != "text") {
    llvm::errs() << "lotus-ir-usehistory: --check requires --format=text\n";
    return 1;
  }
  if (!Check.empty() && Check != "double-free" && Check != "use-after-free") {
    llvm::errs() << "lotus-ir-usehistory: unsupported --check rule\n";
    return 1;
  }

  llvm::LLVMContext context;
  llvm::SMDiagnostic diagnostic;
  auto module = llvm::parseIRFile(Input, diagnostic, context);
  if (!module) {
    diagnostic.print(argv[0], llvm::errs());
    return 1;
  }
  try {
    ICFG icfg;
    ICFGBuilder icfgBuilder(&icfg);
    icfgBuilder.build(module.get());
    SVFGBuilderConfig config;
    config.usePointerAnalysis = true;
    config.buildMSSA = true;
    config.resolveIndirectCalls = true;
    SVFGBuilder builder(config);
    std::unique_ptr<SVFG> svfg(builder.build(&icfg));
    if (!DumpSVFG.empty()) svfg->dump(DumpSVFG);
    auto result = buildUseHistoryFromLotusSVFG(*svfg, *module);
    if (Format == "json") result.graph.printJSON(std::cout);
    else if (Format == "dot") result.graph.printDOT(std::cout);
    else {
      std::cout << "svfg_nodes=" << result.native.nodes.size()
                << " svfg_edges=" << result.native.edges.size()
                << " history_nodes=" << result.graph.nodes().size()
                << " issues=" << result.graph.issues().size() << '\n';
      for (const auto &issue : result.graph.issues())
        std::cout << "issue: " << issue << '\n';
      if (!Check.empty()) {
        DefectKind kind = Check == "double-free" ? DefectKind::DoubleFree :
                                                  DefectKind::UseAfterFree;
        auto report = DefectDetector(result.graph).run(kind);
        std::cout << "check=" << Check << " result=" << statusName(report.result.status)
                  << " witness_edges=" << report.result.edges.size() << '\n';
        if (!report.result.message.empty())
          std::cout << "message: " << report.result.message << '\n';
        for (auto id : report.result.nodes) {
          const auto &node = result.graph.node(id);
          std::cout << "witness_node=" << id << " label=" << node.label;
          if (node.object) std::cout << " object=" << *node.object;
          std::cout << '\n';
        }
      }
      if (query) {
        auto source = result.native.nodes.find(Source);
        auto sink = result.native.nodes.find(Sink);
        if (source == result.native.nodes.end() || sink == result.native.nodes.end())
          throw std::invalid_argument("source or sink SVFG node was not imported");
        Query request;
        request.sources = {source->second};
        request.sinks = {sink->second};
        auto answer = QueryEngine(result.graph).run(request);
        std::cout << "query=" << statusName(answer.status)
                  << " witness_edges=" << answer.edges.size() << '\n';
        if (!answer.message.empty()) std::cout << "message: " << answer.message << '\n';
        for (auto node : answer.nodes)
          std::cout << "witness_node=" << node << '\n';
      }
    }
  } catch (const std::exception &error) {
    llvm::errs() << "lotus-ir-usehistory: " << error.what() << '\n';
    return 1;
  }
  return 0;
}
