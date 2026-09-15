//===----------------------------------------------------------------------===//
//
// Interprocedural function summary construction and translation.
//
//===----------------------------------------------------------------------===//

#include "SymbolicExecution/Core/AnalysisSummary.h"

#include "SymbolicExecution/Core/AnalysisState.h"
#include "SymbolicExecution/Solver/PathCondSolver.h"
#include "SymbolicExecution/Solver/SummarySolverManager.h"

#include <utility>

using namespace SymbolicExecution;

template <typename MapTy, typename FuncTy>
static void forEachMap(MapTy &M, FuncTy Proc) {
  for (auto &P : M) {
    const auto &K = P.first;
    auto &V = P.second;
    Proc(K, V);
  }
}

void AnalysisSummary::translate() {
  // Sync among different functions that are creating summaries
  assert(SolverShared);

  std::lock_guard<std::mutex> LK(SmrySolver->getSolverLock());

  // Translation rewrites every SMT-backed payload in the summary onto the
  // shared summary solver. This is the ownership boundary between an ephemeral
  // AnalysisState and the persistent interprocedural summary object.
  forEachMap(OutSymbolicValMap,
             [&](const ProgramValuePtr &, GuardedSymbolicValSet &V) {
               V.translate(SmrySolver);
             });

  forEachMap(EscapeAllocToSizes,
             [&](const ProgramValuePtr &, GuardedSymbolicValSet &V) {
               V.translate(SmrySolver);
             });

  forEachMap(OutputPts, [&](const ProgramValuePtr &, PtsSet &V) {
    V.translate(SmrySolver);
  });

  StrState.translate(SmrySolver);

  TaintSmry.translate(SmrySolver);

  for (auto &P : QueryToTraces) {
    QuerySet &V = P.first;
    for (auto &P : V) {
      P.first.translate(SmrySolver);
    }
    V.translate(SmrySolver);
  }
}

AnalysisSummary::AnalysisSummary(AnalysisState State)
    : Func(State.F), Graph(State.Graph),
      OutSymbolicValMap(State.OutSymbolicValMap),
      EscapeAllocToSizes(State.EscapeAllocToSizes), OutputPts(State.OutputPts),
      UnknownSyms(State.UnknownSyms), StrState(State.StrState),
      TaintSmry(State.TaintSmry), QueryToTraces(State.QueryToTraces) {
  // Summary construction snapshots the interprocedural portion of an analyzed
  // function. The solver choice determines whether the snapshot can keep the
  // original solver alive or must first translate all stored formulas into a
  // shared solver that outlives the transient AnalysisState.
  auto &Mgr = SummarySolverManager::get();
  if (Mgr.isFuncSolverFull()) {
    SmrySolver = Mgr.getSharedSmrySolver();
    SolverShared = true;
    translate();
  } else {
    SmrySolver = Mgr.getSmrySolver(std::move(State));
    SolverShared = false;
  }
  // doIndex();
}
