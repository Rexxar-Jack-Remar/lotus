/** @file SummarySolverManager.h @brief Solver pool that owns solvers for function summaries. */
#ifndef ANALYSIS_SYMBOLICEXECUTION_SOLVER_SUMMARYSOLVERMANAGER_H
#define ANALYSIS_SYMBOLICEXECUTION_SOLVER_SUMMARYSOLVERMANAGER_H

#include "SymbolicExecution/Core/AnalysisLimit.h"
#include "SymbolicExecution/Solver/PathCondSolver.h"

#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace SymbolicExecution {

class AnalysisState;

/// Process-wide pool of solvers backing interprocedural summaries.
///
/// Summary construction either keeps the solver that built a callee's symbolic
/// state or falls back to a shared solver pool once per-function capacity is
/// exhausted.
class SummarySolverManager {
public:
  static SummarySolverManager &get();
  PathCondSolver *getSharedSmrySolver();
  PathCondSolver *getSmrySolver(AnalysisState State);
  void releaseFuncSolver(const llvm::Function *Func);

  bool isFuncSolverFull() const {
    return FuncSolvers.size() >= AnalysisLimit::MAX_FUNC_SOLVER_LIMIT_V;
  }

  void init(unsigned Num = 128);

private:
  std::mutex Mtx;
  unsigned NextSolverIdx = 0;
  std::vector<std::unique_ptr<PathCondSolver>> Solvers;
  std::unordered_map<const llvm::Function *, std::unique_ptr<PathCondSolver>>
      FuncSolvers;

  SummarySolverManager() = default;
};

} // namespace SymbolicExecution

#endif
