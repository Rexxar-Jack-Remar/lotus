#ifndef ANALYSIS_SYMBOLICEXECUTION_ANALYSISDRIVER_H
#define ANALYSIS_SYMBOLICEXECUTION_ANALYSISDRIVER_H

#include "Analysis/SymbolicExecution/AnalysisState.h"

#include <memory>
#include <mutex>

namespace SymbolicExecution {

class AnalysisDriver {
public:
  AnalysisDriver();
  void runOnFunction(GuardedValueFlowGraph *Graph);
  void runOnModuleParallel(Module *M);
  void runOnModule(Module *M);

  std::vector<std::tuple<AnalysisState::SymexBugType, std::vector<TaintStep>,
                         std::vector<TraceStep>>>
  getBugTraces() const;

  void addSummary(Function *F, const AnalysisSummary &Smry);

  bool hasSummary(Function *F) const;

  const AnalysisSummary &getSummary(Function *F) const;

private:
  unsigned BugTy = AnalysisState::BUG_TY_UNDEF;
  mutable std::mutex AnalysisMtx;
  std::unordered_map<Function *, std::unique_ptr<AnalysisSummary>> AnalysisRes;
  // std::vector<std::vector<std::pair<Instruction *, std::string>>> Traces;
  std::vector<std::tuple<AnalysisState::SymexBugType, std::vector<TaintStep>,
                         std::vector<TraceStep>>>
      Traces;
  std::unordered_set<Instruction *> SinkInsts;

  void initBugType();
  void releaseMemForFunction(Function *F);
};

} // namespace SymbolicExecution

#endif
