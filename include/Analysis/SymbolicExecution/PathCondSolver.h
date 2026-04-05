#ifndef ANALYSIS_SYMBOLICEXECUTION_PATHCONDSOLVER_H
#define ANALYSIS_SYMBOLICEXECUTION_PATHCONDSOLVER_H

#include "Analysis/SymbolicExecution/BigInteger.h"
#include "Analysis/SymbolicExecution/PropertyValue.h"
#include "Analysis/SymbolicExecution/SegUtility.h"
#include "IR/GVFG/GuardedValueFlowSolver.h"
#include "Solvers/SMT/LIBSMT/SMTFactory.h"

#include <atomic>
#include <memory>
#include <vector>

namespace SymbolicExecution {

/// Builds and checks SMT formulas for SymbolicExecution path conditions.
///
/// The solver is the bridge between Lotus symbolic values and SMT terms. It
/// translates PropertyValue based expressions, region guards, and dependence
/// information from the GVFG into formulas that can be reused across many
/// satisfiability queries during one analysis run.

using llvm::BasicBlock;
using llvm::Function;
using lotus::gvfg::GuardedValueFlowCallOutputNode;
using lotus::gvfg::GuardedValueFlowGraph;
using lotus::gvfg::GuardedValueFlowNode;
using lotus::gvfg::GuardedValueFlowPhiNode;
using lotus::gvfg::GuardedValueFlowRegionNode;
using lotus::gvfg::GuardedValueFlowSolver;
class Query;
class PropertySymExpr;

/// Solver owned context for constructing and checking symbolic path predicates.
///
/// A PathCondSolver caches the SMT factory, dependence queries, and auxiliary
/// state needed to express symbolic access path constraints. AnalysisState and
/// summaries use separate solver instances when they need isolated ownership of
/// expressions, and translate conditions when moving facts across solver
/// boundaries.
class PathCondSolver {
public:
  PathCondSolver();
  ~PathCondSolver() = default;
  PathCondSolver(const PathCondSolver &) = delete;
  PathCondSolver &operator=(const PathCondSolver &) = delete;

  SMTExpr buildRegionCondition(GuardedValueFlowRegionNode *Cond);
  SMTExprVec createEmptySMTExprVec();
  SMTExpr buildExprForVal(const PropertyValue *V);
  SMTExpr buildBitVecVal(uint64_t V, uint64_t Sz);
  SMTExpr getExpr(Var V);
  SMTExpr renameExpr(const SMTExpr &Constr, const std::string &Suffix);
  SMTExpr buildBoolVal(bool V);
  SMTExpr buildBoolUnknown(const std::string &Name);
  SMTExpr buildPredicate(const PropertyValue *Va1, const PropertyValue *Va2,
                         unsigned Pred);
  SMTExpr buildEqualCond(Var V, const PropertyValue *Va,
                         const std::string &VSuffix = "");
  SMTExpr getPhiGated(const GuardedValueFlowPhiNode *PhiNode,
                      const GuardedValueFlowPhiNode::Incoming InNode);

  SMTExpr getCtrlDeps(BasicBlock *B, const GuardedValueFlowGraph *G);

  SMTExpr getDataDeps(const GuardedValueFlowNode *N);

  std::unordered_set<const GuardedValueFlowCallOutputNode *>
  getUsedCallSiteOutput() const {
    return UsedCSOuts;
  }

  bool isConstraintSat(const SMTExprVec &Cons);

  /// Monadic Predicate Abstraction (MPA): Batch satisfiability checking
  /// Given a fixed context φ and a set of predicates P = {p₁, p₂, ..., pₙ},
  /// determines for each predicate pᵢ whether φ ∧ pᵢ is satisfiable.
  ///
  /// This method is optimized for batch checking by:
  /// 1. Pushing the shared context once
  /// 2. Checking each predicate incrementally
  /// 3. Reusing solver state (learned clauses, theory lemmas) across queries
  ///
  /// \param Context The shared symbolic context φ (path condition)
  /// \param Predicates Vector of predicates {p₁, p₂, ..., pₙ} to check
  /// \return Vector of boolean results, where result[i] indicates whether
  ///         Context ∧ Predicates[i] is satisfiable
  std::vector<bool>
  batchCheckPredicates(const SMTExprVec &Context,
                       const std::vector<SMTExprVec> &Predicates);

  /// MPA with predicate relationship analysis
  /// Analyzes relationships among predicates (implications, mutual exclusion)
  /// to prune queries before invoking the solver.
  ///
  /// \param Context The shared symbolic context φ
  /// \param Predicates Vector of predicates to check
  /// \param Results Output vector of satisfiability results
  /// \param PrunedCount Output count of queries pruned without solver calls
  void batchCheckPredicatesWithAnalysis(
      const SMTExprVec &Context, const std::vector<SMTExprVec> &Predicates,
      std::vector<bool> &Results, unsigned &PrunedCount);

  SMTExpr translate(const SMTExpr &Cons) { return Fctry.translate(Cons); }

  std::mutex &getSolverLock() { return Mtx; }

  unsigned getExprSize(const SMTExpr &E, unsigned LimitVal);

  uint32_t getID() const { return SolverID; }

private:
  static std::atomic<uint32_t> ID;

  uint32_t SolverID;
  std::mutex Mtx;
  SMTFactory Fctry;
  std::unordered_set<const GuardedValueFlowCallOutputNode *> UsedCSOuts;
  std::unique_ptr<GuardedValueFlowSolver> GraphSolver;

  std::unordered_map<unsigned, unsigned> ConstrToSize;
  std::unordered_map<
      unsigned, std::unordered_map<Function *, std::unordered_set<unsigned>>>
      ConstrToCSFormals;

  void resetSolverState();
  void updateUsedCSOuts();
  void normalizeExprWidth(SMTExpr &L, SMTExpr &R);
  void normalizeExprWidth(SMTExpr &E1, SMTExpr &E2, SMTExpr &E3);
  SMTExpr buildSymExpr(const PropertySymExpr *V);
  SMTExpr buildIntExpr(const BigInteger &I);
};
} // namespace SymbolicExecution
#endif
