//===----------------------------------------------------------------------===//
//
// Solver pool backing interprocedural function summaries.
//
//===----------------------------------------------------------------------===//

#include "SymbolicExecution/Solver/SummarySolverManager.h"

#include "SymbolicExecution/Core/AnalysisState.h"
#include "SymbolicExecution/Solver/PathCondSolver.h"

#include <cassert>
#include <utility>

using namespace SymbolicExecution;

void SummarySolverManager::init(unsigned Num) {
  assert(Num > 0);

  Solvers.resize(Num);
  for (unsigned Idx = 0; Idx < Num; ++Idx) {
    Solvers[Idx] = std::unique_ptr<PathCondSolver>(new PathCondSolver());
  }
}

SummarySolverManager &SummarySolverManager::get() {
  static SummarySolverManager Mgr;
  return Mgr;
}

PathCondSolver *SummarySolverManager::getSharedSmrySolver() {
  std::lock_guard<std::mutex> LK(Mtx);

  // Shared summary solvers are used once the dedicated per-function solver pool
  // is full. Round-robin reuse keeps summary materialization progressing, but
  // any summary placed on a shared solver must be translated away from the
  // original AnalysisState-owned solver before that state can be discarded.
  auto *Res = Solvers[NextSolverIdx].get();
  NextSolverIdx = (NextSolverIdx + 1) % Solvers.size();
  return Res;
}

PathCondSolver *SummarySolverManager::getSmrySolver(AnalysisState State) {
  std::lock_guard<std::mutex> LK(Mtx);

  // When capacity allows, a summary keeps the solver that built its symbolic
  // state. That avoids immediate translation and lets later imports reuse the
  // exact SMT objects created during the callee analysis.
  auto *Func = State.F;
  FuncSolvers.insert(std::make_pair(Func, std::move(State.Solver)));
  return FuncSolvers.at(Func).get();
}

void SummarySolverManager::releaseFuncSolver(const llvm::Function *Func) {
  std::lock_guard<std::mutex> LK(Mtx);
  FuncSolvers.erase(Func);
}
