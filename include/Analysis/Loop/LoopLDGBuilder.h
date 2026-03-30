/*
 * Copyright 2026 Lotus contributors
 */
#ifndef LOTUS_ANALYSIS_LOOP_LOOPLDGBUILDER_H
#define LOTUS_ANALYSIS_LOOP_LOOPLDGBUILDER_H

#include "Analysis/Loop/LoopDependenceGraph.h"
#include "Analysis/Loop/LoopSCCDAG.h"

#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/ScalarEvolution.h"

namespace lotus {
namespace analysis {
namespace loop {

struct LoopLDGBuilderOptions {
  bool enableLoopAwareDependenceAnalyses{true};
  bool hasLoopAwareDependenceBackend{true};
  bool enableAffineIterationSpaceRefinement{true};
  bool enableMemoryCloningRefinement{true};
  bool enableThreadSafeLibraryRefinement{true};
};

class LoopLDGBuilder {
public:
  struct GraphBundle {
    std::unique_ptr<LoopDependenceGraph> graph;
    std::unique_ptr<LoopSCCDAG> sccdag;
    std::vector<std::pair<std::string, std::string>> debugSnapshots;
  };

  static GraphBundle buildBaseLoopDependenceGraph(LoopTree *loopNode,
                                                  pdg::ProgramGraph &pdg);

  static GraphBundle refineLoopDependenceGraph(
      std::unique_ptr<LoopDependenceGraph> graph,
      llvm::ScalarEvolution &SE,
      llvm::LoopInfo &LI,
      const noelle::DominatorSummary &DS,
      LoopLDGBuilderOptions options = {});

private:
  static void captureSnapshot(
      GraphBundle &bundle,
      const std::string &phase,
      const LoopDependenceGraph &graph);
};

} // namespace loop
} // namespace analysis
} // namespace lotus

#endif
