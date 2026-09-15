/** @file AnalysisSummary.h @brief Interprocedural function summary exported by symbolic execution. */
#ifndef ANALYSIS_SYMBOLICEXECUTION_CORE_ANALYSISSUMMARY_H
#define ANALYSIS_SYMBOLICEXECUTION_CORE_ANALYSISSUMMARY_H

#include "SymbolicExecution/Core/CStringState.h"
#include "SymbolicExecution/Core/GuardedValue.h"
#include "SymbolicExecution/Core/Query.h"
#include "SymbolicExecution/Core/TaintState.h"
#include "SymbolicExecution/Core/Trace.h"

#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace SymbolicExecution {

class AnalysisState;

/// Interprocedural summary exported from one analyzed function.
///
/// The summary stores the observable state needed by callers: symbolic return
/// values, escaped object sizes, points to facts, taint information, and the
/// query traces that justify reported bugs. Conditions are translated into a
/// summary owned solver so the summary can outlive the local AnalysisState.
class AnalysisSummary {
  friend class AnalysisState;

private:
  // The solver context that owns smt exprs of this AnalysisSummary
  PathCondSolver *SmrySolver = nullptr;
  // Is SmrySolver shared by other AnalysisSummarys?
  bool SolverShared = false;

  // The function this summary is created for
  Function *Func = nullptr;
  GuardedValueFlowGraph *Graph = nullptr;

  // output val -> corresponding symbolic value in reg
  std::unordered_map<ProgramValuePtr, GuardedSymbolicValSet> OutSymbolicValMap;
  // alloc site of an escape object -> size of the object
  std::unordered_map<ProgramValuePtr, GuardedSymbolicValSet> EscapeAllocToSizes;
  // Points to result of output val
  std::unordered_map<ProgramValuePtr, PtsSet> OutputPts;
  std::unordered_set<ProgramValuePtr> UnknownSyms;
  CStringState StrState;
  TaintSummary TaintSmry;
  std::vector<std::pair<QuerySet, std::vector<TraceStep>>> QueryToTraces;

  void translate();

public:
  AnalysisSummary(AnalysisState State);

  bool isSolverShared() const { return SolverShared; }

  Function *getFunc() const { return Func; }

  PathCondSolver *getSmrySolver() const { return SmrySolver; }

  const decltype(OutSymbolicValMap) &getOutSymbolicValMap() const {
    return OutSymbolicValMap;
  }

  const decltype(EscapeAllocToSizes) &getEscapeInfo() const {
    return EscapeAllocToSizes;
  }

  const TaintSummary &getTaintSmry() const { return TaintSmry; }

  const decltype(QueryToTraces) &getQueryToTraces() const {
    return QueryToTraces;
  }

  const decltype(UnknownSyms) &getUnknownSyms() const { return UnknownSyms; }

  const decltype(StrState) &getStrState() const { return StrState; }

  GuardedValueFlowGraph *getGraph() const { return Graph; }
};

} // namespace SymbolicExecution

#endif
