/*
 * Copyright 2026 Lotus contributors
 */
#ifndef LOTUS_ANALYSIS_LOOP_LOOPLDGBUILDER_H
#define LOTUS_ANALYSIS_LOOP_LOOPLDGBUILDER_H

#include "Analysis/Loop/LoopDependenceGraph.h"
#include "Analysis/Loop/LoopSCCDAG.h"

#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/ScalarEvolution.h"

#include <vector>

namespace lotus {
namespace analysis {
namespace loop {

class LoopAwareDependenceRefinementPass {
public:
  virtual ~LoopAwareDependenceRefinementPass() = default;

  virtual void refine(LoopDependenceGraph &graph,
                      LoopTree *loopNode,
                      llvm::ScalarEvolution &SE,
                      llvm::LoopInfo &LI,
                      const noelle::DominatorSummary &DS) = 0;
};

struct LoopLDGBuilderOptions {
  bool enableLoopAwareDependenceAnalyses{true};
  bool hasLoopAwareDependenceBackend{true};
  bool enableAffineIterationSpaceRefinement{true};
  bool enableMemoryCloningRefinement{true};
  bool enableThreadSafeLibraryRefinement{true};
  bool enableExtendedIVRecognition{false};
  bool assumePseudoRandomValueGeneratorsNonDeterministic{false};
  std::vector<LoopAwareDependenceRefinementPass *> loopAwareRefinementPasses;

  void addLoopAwareRefinementPass(LoopAwareDependenceRefinementPass *pass);
  void removeLoopAwareRefinementPass(LoopAwareDependenceRefinementPass *pass);
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

  static std::unique_ptr<LoopDependenceGraph>
  createInternalSubgraph(const LoopDependenceGraph &graph);

  static std::unique_ptr<LoopSCCDAG>
  computeSCCDAGWithOnlyVariableAndControlDependences(
      const LoopDependenceGraph &graph);

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
