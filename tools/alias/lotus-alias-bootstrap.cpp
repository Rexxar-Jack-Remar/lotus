/**
 * @file lotus-alias-bootstrap.cpp
 * @brief Driver for bootstrapped flow- and context-sensitive pointer analysis.
 */
#include "Alias/InclusionBased/BootstrapAA/BootstrapAA.h"

#include <exception>

#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/InitLLVM.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>

namespace {
llvm::cl::OptionCategory
    BootstrapCategory("Lotus bootstrapped pointer analysis");
llvm::cl::opt<std::string> Input(llvm::cl::Positional, llvm::cl::Required,
                                 llvm::cl::desc("<input LLVM IR or bitcode>"),
                                 llvm::cl::cat(BootstrapCategory));
llvm::cl::opt<std::string>
    Entry("entry", llvm::cl::init("main"),
          llvm::cl::desc("Defined program entry function"),
          llvm::cl::cat(BootstrapCategory));
llvm::cl::opt<bool> AllContexts(
    "all-contexts", llvm::cl::init(false),
    llvm::cl::desc("Print the union of reachable contexts in every function"),
    llvm::cl::cat(BootstrapCategory));
llvm::cl::opt<unsigned>
    Threshold("andersen-threshold", llvm::cl::init(60),
              llvm::cl::desc("Andersen refinement threshold"),
              llvm::cl::cat(BootstrapCategory));
llvm::cl::opt<unsigned>
    ContextLimit("max-contexts", llvm::cl::init(4096),
                 llvm::cl::desc("Maximum summary contexts per cluster"),
                 llvm::cl::cat(BootstrapCategory));
llvm::cl::opt<unsigned>
    StepLimit("max-steps", llvm::cl::init(1000000),
              llvm::cl::desc("Maximum solver steps per cluster"),
              llvm::cl::cat(BootstrapCategory));
llvm::cl::opt<bool> DetailedStats(
    "detailed-stats", llvm::cl::init(false),
    llvm::cl::desc("Print partition and cluster size distributions"),
    llvm::cl::cat(BootstrapCategory));
} // namespace

int main(int argc, char **argv) {
  llvm::InitLLVM init(argc, argv);
  llvm::cl::HideUnrelatedOptions(BootstrapCategory);
  llvm::cl::ParseCommandLineOptions(
      argc, argv, "Bootstrapped flow/context-sensitive points-to analysis\n");
  llvm::LLVMContext context;
  llvm::SMDiagnostic diagnostic;
  auto module = llvm::parseIRFile(Input, diagnostic, context);
  if (!module) {
    diagnostic.print(argv[0], llvm::errs());
    return 1;
  }
  const llvm::Function *entry = module->getFunction(Entry);
  if (!entry || entry->isDeclaration()) {
    llvm::errs() << "No defined entry function: " << Entry << '\n';
    return 1;
  }
  try {
    lotus::bootstrap::Options options;
    options.andersen_threshold = Threshold;
    options.max_contexts = ContextLimit;
    options.max_steps = StepLimit;
    lotus::bootstrap::BootstrapAA analysis(*module, entry, options);
    bool limited = false;
    for (const llvm::Function &function : *module) {
      if (!AllContexts && &function != entry)
        continue;
      for (const llvm::BasicBlock &block : function) {
        for (const llvm::Instruction &instruction : block) {
          if (!instruction.getType()->isPointerTy())
            continue;
          auto result =
              AllContexts
                  ? analysis.pointsToAllContexts(instruction, instruction,
                                                 lotus::bootstrap::Point::After)
                  : analysis.pointsTo(instruction, instruction, {},
                                      lotus::bootstrap::Point::After);
          llvm::outs() << function.getName() << ": ";
          instruction.printAsOperand(llvm::outs(), false);
          llvm::outs() << " -> ";
          if (!result.reachable()) {
            llvm::outs() << "<unreachable>\n";
            continue;
          }
          llvm::outs() << '{';
          bool first = true;
          for (lotus::bootstrap::Id object : result.points_to.objects()) {
            if (!first)
              llvm::outs() << ", ";
            first = false;
            llvm::outs() << analysis.objectInfo(object).name;
          }
          llvm::outs() << '}';
          if (result.status == lotus::bootstrap::QueryStatus::ResourceLimit) {
            limited = true;
            llvm::outs() << " [resource-limit fallback]";
          }
          llvm::outs() << '\n';
        }
      }
    }
    const auto &stats = analysis.statistics();
    llvm::errs() << "partitions=" << stats.steensgaard_partitions
                 << " largest-partition=" << stats.steensgaard_largest_partition
                 << " hierarchy-nodes=" << stats.hierarchy_nodes
                 << " hierarchy-edges=" << stats.hierarchy_edges
                 << " hierarchy-depth=" << stats.hierarchy_max_depth
                 << " cyclic-components=" << stats.hierarchy_cyclic_components
                 << " andersen-runs=" << stats.andersen_runs
                 << " clusters=" << stats.clusters
                 << " largest-cluster=" << stats.largest_cluster
                 << " cover-memberships=" << stats.cover_memberships
                 << " overlapping-values=" << stats.overlapping_values
                 << " max-cover-memberships=" << stats.maximum_cover_memberships
                 << " evaluated-clusters=" << stats.evaluated_clusters
                 << " contexts=" << stats.contexts
                 << " context-cache-hits=" << stats.context_cache_hits
                 << " context-cache-misses=" << stats.context_cache_misses
                 << " max-contexts-per-cluster="
                 << stats.maximum_contexts_per_cluster
                 << " summary-updates=" << stats.summary_updates
                 << " preprocessing-ms=" << stats.preprocessing_milliseconds
                 << " steps=" << stats.steps
                 << " fallback-queries=" << stats.fallback_queries << '\n';
    auto printSizes = [](llvm::StringRef label,
                         const std::vector<std::size_t> &sizes) {
      llvm::errs() << label << "={";
      for (std::size_t index = 0; index < sizes.size(); ++index) {
        if (index)
          llvm::errs() << ',';
        llvm::errs() << sizes[index];
      }
      llvm::errs() << "}\n";
    };
    if (DetailedStats) {
      printSizes("steensgaard-partition-sizes",
                 stats.steensgaard_partition_sizes);
      printSizes("andersen-cluster-sizes", stats.cluster_sizes);
      llvm::errs() << "cluster-solve-ms={";
      for (std::size_t index = 0;
           index < stats.cluster_solve_milliseconds.size(); ++index) {
        if (index)
          llvm::errs() << ',';
        llvm::errs() << stats.cluster_solve_milliseconds[index];
      }
      llvm::errs() << "}\n";
    }
    return limited ? 2 : 0;
  } catch (const std::exception &error) {
    llvm::errs() << "BootstrapAA: " << error.what() << '\n';
    return 1;
  }
}
