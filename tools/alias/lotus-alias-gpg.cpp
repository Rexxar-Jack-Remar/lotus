#include "Alias/InclusionBased/GPG/Analysis.h"
#include "Alias/Infrastructure/AliasAnalysisWrapper/CLIUtils.h"

#include <memory>
#include <string>

#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/InitLLVM.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;
using namespace lotus::alias::tools;
using namespace lotus::gpg;

namespace {

enum class ModeOption { FSCS, FICS, FICI };

cl::OptionCategory GPGCategory("Lotus GPG points-to analysis");

cl::opt<std::string> InputFilename(cl::Positional,
                                   cl::desc("<input bitcode file>"),
                                   cl::Required, cl::cat(GPGCategory));

cl::opt<unsigned>
    HeapKLimit("heap-k",
               cl::desc("k-limit for live-on-entry heap indirection lists"),
               cl::init(3), cl::cat(GPGCategory));

cl::opt<ModeOption>
    Mode("mode", cl::desc("Points-to analysis sensitivity"),
         cl::values(clEnumValN(ModeOption::FSCS, "fscs",
                               "Flow- and context-sensitive (default)"),
                    clEnumValN(ModeOption::FICS, "fics",
                               "Flow-insensitive, context-sensitive"),
                    clEnumValN(ModeOption::FICI, "fici",
                               "Flow- and context-insensitive")),
         cl::init(ModeOption::FSCS), cl::cat(GPGCategory));

cl::opt<bool> ArrayIndexSensitive(
    "array-index-sensitive",
    cl::desc("Distinguish constant array indices (struct fields are always "
             "distinguished)"),
    cl::init(false), cl::cat(GPGCategory));

cl::opt<bool>
    DisableBlocking("disable-blocking",
                    cl::desc("Disable barrier-aware reaching-GPU analysis"),
                    cl::init(false), cl::cat(GPGCategory));

cl::opt<bool> DisableDeadGPU("disable-dead-gpu-elimination",
                             cl::desc("Disable dead-GPU elimination"),
                             cl::init(false), cl::cat(GPGCategory));

cl::opt<bool> DisableCoalescing("disable-coalescing",
                                cl::desc("Disable GPB coalescing"),
                                cl::init(false), cl::cat(GPGCategory));

cl::opt<bool>
    PrintPointsTo("print-pts",
                  cl::desc("Print statement-specific points-to information"),
                  cl::init(false), cl::cat(GPGCategory));

cl::opt<bool> PrintCallGraph("print-call-graph",
                             cl::desc("Print resolved indirect-call targets"),
                             cl::init(false), cl::cat(GPGCategory));

cl::opt<bool> PrintModRef("print-modref",
                          cl::desc("Print per-function mod/ref summaries"),
                          cl::init(false), cl::cat(GPGCategory));

cl::opt<bool> PrintStats("print-stats", cl::desc("Print analysis statistics"),
                         cl::init(true), cl::cat(GPGCategory));

void printAccess(const Access &access, const ProgramModel &model) {
  outs() << model.locationName(access.location);
  if (access.upward_exposed)
    outs() << '\'';
  outs() << access.indirections.str();
}

} // namespace

int main(int argc, char **argv) {
  InitLLVM init(argc, argv);
  cl::HideUnrelatedOptions(GPGCategory);
  cl::ParseCommandLineOptions(
      argc, argv,
      "GPG flow-, field-, and context-sensitive points-to analysis\n");

  LLVMContext context;
  SMDiagnostic diagnostic;
  std::unique_ptr<Module> module =
      loadIRModule(InputFilename, context, diagnostic, argv[0]);
  if (!module)
    return 1;

  GPGConfig config;
  switch (Mode) {
  case ModeOption::FSCS:
    config.mode = AnalysisMode::FlowAndContextSensitive;
    break;
  case ModeOption::FICS:
    config.mode = AnalysisMode::FlowInsensitiveContextSensitive;
    break;
  case ModeOption::FICI:
    config.mode = AnalysisMode::FlowAndContextInsensitive;
    break;
  }
  config.heap_indirection_limit = HeapKLimit;
  config.array_index_sensitive = ArrayIndexSensitive;
  config.enable_blocking = !DisableBlocking;
  config.enable_dead_gpu_elimination = !DisableDeadGPU;
  config.enable_coalescing = !DisableCoalescing;

  GPGAnalysisEngine analysis(*module, config);
  analysis.run();
  const GPGResult &result = analysis.result();
  const ProgramModel &model = analysis.programModel();

  if (PrintPointsTo) {
    for (const auto &[instruction, queries] : result.pointsToInformation()) {
      outs() << instruction->getFunction()->getName() << ":";
      if (instruction->hasName())
        outs() << instruction->getName();
      else
        outs() << model.statementId(instruction);
      outs() << "\n";
      for (const auto &[query, resolved] : queries) {
        outs() << "  pts(";
        printAccess(query, model);
        outs() << ") = {";
        bool first = true;
        for (const Access &target : resolved) {
          if (!first)
            outs() << ", ";
          first = false;
          printAccess(target, model);
        }
        outs() << "}\n";
      }
    }
  }

  if (PrintCallGraph) {
    for (const auto &[call, targets] : result.indirectCallTargets()) {
      outs() << call->getFunction()->getName() << ":";
      if (call->hasName())
        outs() << call->getName();
      else
        outs() << model.statementId(call);
      outs() << " -> {";
      bool first = true;
      for (const Function *target : targets) {
        if (!first)
          outs() << ", ";
        first = false;
        outs() << target->getName();
      }
      outs() << "}\n";
    }
  }

  if (PrintModRef) {
    for (const auto &[function, summary] : result.modRefSummaries()) {
      outs() << function->getName() << " mod={";
      bool first = true;
      for (const Access &access : summary.modifications) {
        if (!first)
          outs() << ", ";
        first = false;
        printAccess(access, model);
      }
      outs() << "} ref={";
      first = true;
      for (const Access &access : summary.references) {
        if (!first)
          outs() << ", ";
        first = false;
        printAccess(access, model);
      }
      outs() << "}\n";
    }
  }

  if (PrintStats) {
    const AnalysisStats &stats = result.stats();
    outs() << "gpg.functions=" << stats.functions << "\n"
           << "gpg.initial-gpbs=" << stats.initial_gpbs << "\n"
           << "gpg.initial-gpus=" << stats.initial_gpus << "\n"
           << "gpg.optimized-gpbs=" << stats.optimized_gpbs << "\n"
           << "gpg.optimized-gpus=" << stats.optimized_gpus << "\n"
           << "gpg.indirect-calls=" << stats.indirect_calls << "\n"
           << "gpg.resolved-indirect-calls=" << stats.resolved_indirect_calls
           << "\n"
           << "gpg.call-graph-refinements=" << stats.call_graph_refinements
           << "\n";
  }
  return 0;
}
