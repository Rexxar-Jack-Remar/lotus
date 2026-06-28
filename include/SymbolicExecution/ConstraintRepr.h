/** @file ConstraintRepr.h @brief Constraint representation for symbolic execution path conditions. */
#ifndef ANALYSIS_SYMBOLICEXECUTION_CONSTRAINT_REPR_H
#define ANALYSIS_SYMBOLICEXECUTION_CONSTRAINT_REPR_H

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instruction.h"

#include "SymbolicExecution/AnalysisLimit.h"
#include "SymbolicExecution/PropertyAllocator.h"
#include "IR/GVFG/GuardedValueFlowGraph.h"
#include "Solvers/SMT/LIBSMT/SMTExpr.h"

#include <map>
#include <memory>
#include <set>
#include <unordered_map>
#include <vector>

namespace SymbolicExecution {

/// Encodes symbolic path conditions used by the symbolic execution engine.
///
/// This header keeps the public condition abstraction lightweight. Most clients
/// manipulate Condition values symbolically and only materialize solver terms
/// when a query needs to be discharged.

using lotus::gvfg::GuardedValueFlowRegionNode;

class PathCondSolver;

class AnalysisState;

/// A solver backed boolean condition attached to symbolic states and sets.
///
/// Conditions are either literals or SMT expressions owned by a
/// PathCondSolver. They serve as the shared guard representation for symbolic
/// values, points to facts, and bug queries, which lets the analysis combine
/// dataflow style joins with path sensitive reasoning.
class SMTCondition {
public:
  SMTCondition() = default;
  SMTCondition(bool V);
  SMTCondition(GuardedValueFlowRegionNode *Cond, PathCondSolver *Solver);
  SMTCondition(const SMTExpr &Constr, PathCondSolver *Solver);
  SMTCondition(PropertyValuePtr L, PropertyValuePtr R, CmpInst::Predicate Pred,
               PathCondSolver *Solver);

  static SMTCondition getFalseCond() { return SMTCondition(false); }

  static SMTCondition getTrueCond() { return SMTCondition(true); }

  SMTCondition(const SMTCondition &R)
      : Index(R.Index), Lit(R.Lit), Solver(R.Solver) {
    if (R.Constr) {
      Constr = std::unique_ptr<SMTExpr>(new SMTExpr(*R.Constr));
    }
  }

  SMTCondition(SMTCondition &&R)
      : Index(R.Index), Lit(R.Lit), Constr(std::move(R.Constr)),
        Solver(R.Solver) {}

  SMTCondition &operator=(SMTCondition Tmp) {
    std::swap(Index, Tmp.Index);
    std::swap(Lit, Tmp.Lit);
    std::swap(Constr, Tmp.Constr);
    std::swap(Solver, Tmp.Solver);
    return *this;
  }

  bool isTrue() const;
  bool isFalse() const;

  SMTCondition operator||(const SMTCondition &R) const;
  SMTCondition operator&&(const SMTCondition &R) const;
  SMTCondition operator!() const;

  // Utilities for enabling constraint as map key
  bool operator==(const SMTCondition &R) const;
  bool operator!=(const SMTCondition &R) const;
  bool operator<(const SMTCondition &R) const;
  size_t hash() const;

  void orCond(const SMTCondition &R) { *this = operator||(R); }

  void andCond(const SMTCondition &R) { *this = operator&&(R); }

  bool isLit() const {
    if (Lit != -1) {
      return true;
    } else {
      return false;
    }
  }

  int64_t getIndex() const;

  SMTCondition translate(PathCondSolver *NewSolver, bool &ToUnknown) const;

  SMTCondition translateNoLock(PathCondSolver *NewSolver) const;

  SMTCondition rename(const std::string &Suffix) const;

  SMTExpr getSMTConstr() const {
    assert(!isLit());
    return *Constr;
  }

  bool isConstrComplex() const;
  SMTExprVec toSMT(PathCondSolver &S) const;
  void dump() const;

private:
  int64_t Index = -3;

  // 1 => true; 0 => false; -1 => not a literal
  int Lit = 1;
  // For literal, Constr == nullptr and Solver == nullptr
  std::unique_ptr<SMTExpr> Constr = nullptr;
  PathCondSolver *Solver = nullptr;
};

using Condition = SMTCondition;

} // namespace SymbolicExecution

#endif
