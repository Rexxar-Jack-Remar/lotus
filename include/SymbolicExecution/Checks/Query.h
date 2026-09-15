/** @file Query.h @brief Bug query hierarchy discharged against symbolic state. */
#ifndef ANALYSIS_SYMBOLICEXECUTION_CHECKS_QUERY_H
#define ANALYSIS_SYMBOLICEXECUTION_CHECKS_QUERY_H

#include "SymbolicExecution/Core/GuardedValue.h"
#include "SymbolicExecution/Core/PropertyAllocator.h"
#include "SymbolicExecution/Core/PropertyValue.h"
#include "SymbolicExecution/Core/Query.h"
#include "SymbolicExecution/Integration/GVFGUtility.h"
#include "SymbolicExecution/Solver/PathCondSolver.h"

#include "Solvers/SMT/LIBSMT/SMTExpr.h"

#include <memory>
#include <vector>

namespace SymbolicExecution {

class NumericalQueryPtr;

/// Abstract bug query discharged against the current symbolic state.
///
/// Queries capture the safety property that should hold at a program point.
/// Some queries ask about a single symbolic expression, while others reason
/// about a symbolic access path together with an access size.
class NumericalQuery {
  friend class NumericalQueryPtr;

public:
  enum QueryKind { QK_DIRECT, QK_INDIRECT };

  NumericalQuery(QueryKind QK, unsigned BugTy, const GuardedProgramValSet &Deps)
      : QK(QK), BugTy(BugTy), Deps(Deps) {}

  virtual ~NumericalQuery() {}

  QueryKind getKind() const { return QK; }

  static bool classof(const NumericalQuery *) { return true; }

  virtual GuardedProgramValSet getUsedVals() const = 0;

  NumericalQueryPtr clone() const;

  virtual void dump() const {}

  virtual size_t hash() const = 0;

  const GuardedProgramValSet &getDeps() const { return Deps; }

  unsigned getBugTy() const { return BugTy; }

protected:
  // AnalysisState::SymexBugType
  QueryKind QK;
  unsigned BugTy = 0x0;
  mutable GuardedProgramValSet Deps;

  void dumpDeps() const;

private:
  void translate(PathCondSolver *Solver) const { Deps.translate(Solver); }
};

/// Query over one symbolic predicate or value expression.
///
/// Direct queries are used when the bug check can be phrased as a pure numeric
/// condition, such as divisor non zero or overflow guards for an arithmetic
/// expression.
class DirectNumericalQuery : public NumericalQuery {
  DirectNumericalQuery(unsigned BugTy, const PropertyValuePtr &Qr, bool Eq,
                       const GuardedProgramValSet &Deps)
      : NumericalQuery(QK_DIRECT, BugTy, Deps), Q(Qr), Eq(Eq) {
    if (Q) {
      assert(IsaProperty<PropertySymExpr>(Q));
    }
  }

  // Default constructed query "must sat"
  DirectNumericalQuery(unsigned BugTy) : NumericalQuery(QK_DIRECT, BugTy, {}) {}

  // The query expression cannot be resolved due to unknown
  // access size.
  // For example:
  // char dest[30];
  // strcpy(dest, argv[1]);
  // In such case, we should add argv[1] to Deps
  // and report a bug if it is tainted.
  DirectNumericalQuery(unsigned BugTy, const GuardedProgramValSet &Deps)
      : NumericalQuery(QK_DIRECT, BugTy, Deps) {}

public:
  virtual ~DirectNumericalQuery() {}

  static NumericalQueryPtr getBofMustErrQuery();

  static NumericalQueryPtr
  getBofTaintOnlyQuery(const GuardedProgramValSet &Deps);

  static NumericalQueryPtr
  getBofSymbolicQuery(const PropertyValuePtr &Qr,
                      const GuardedProgramValSet &Deps);

  static NumericalQueryPtr getDbzMustErrQuery();

  static NumericalQueryPtr
  getDbzSymbolicQuery(const PropertyValuePtr &Qr,
                      const GuardedProgramValSet &Deps);

  static NumericalQueryPtr getIntOverflowMustErrQuery();
  static NumericalQueryPtr
  getIntOverflowSymbolicQuery(const PropertyValuePtr &Qr,
                              const GuardedProgramValSet &Deps);

  static NumericalQueryPtr getIntUnderflowMustErrQuery();
  static NumericalQueryPtr
  getIntUnderflowSymbolicQuery(const PropertyValuePtr &Qr,
                               const GuardedProgramValSet &Deps);

  static NumericalQueryPtr getNullDerefMustErrQuery();
  static NumericalQueryPtr
  getNullDerefSymbolicQuery(const PropertyValuePtr &Qr,
                            const GuardedProgramValSet &Deps);

  static NumericalQueryPtr getSignedIntOverflowMustErrQuery();
  static NumericalQueryPtr
  getSignedIntOverflowSymbolicQuery(const PropertyValuePtr &Qr,
                                    const GuardedProgramValSet &Deps);

  static NumericalQueryPtr getSignedIntUnderflowMustErrQuery();
  static NumericalQueryPtr
  getSignedIntUnderflowSymbolicQuery(const PropertyValuePtr &Qr,
                                     const GuardedProgramValSet &Deps);

  static NumericalQueryPtr getShiftOverflowMustErrQuery();
  static NumericalQueryPtr
  getShiftOverflowSymbolicQuery(const PropertyValuePtr &Qr,
                                const GuardedProgramValSet &Deps);

  static NumericalQueryPtr getArrayIndexOOBMustErrQuery();
  static NumericalQueryPtr
  getArrayIndexOOBSymbolicQuery(const PropertyValuePtr &Qr,
                                const GuardedProgramValSet &Deps);

  static NumericalQueryPtr getUninitializedReadQuery();

  static NumericalQueryPtr getUafMustErrQuery();
  static NumericalQueryPtr
  getUafSymbolicQuery(const PropertyValuePtr &Qr,
                      const GuardedProgramValSet &Deps);

  static NumericalQueryPtr getDoubleFreeMustErrQuery();
  static NumericalQueryPtr
  getDoubleFreeSymbolicQuery(const PropertyValuePtr &Qr,
                             const GuardedProgramValSet &Deps);

  static NumericalQueryPtr getNegativeArrayIndexMustErrQuery();
  static NumericalQueryPtr
  getNegativeArrayIndexSymbolicQuery(const PropertyValuePtr &Qr,
                                     const GuardedProgramValSet &Deps);

  static NumericalQueryPtr getIntTruncationMustErrQuery();
  static NumericalQueryPtr
  getIntTruncationSymbolicQuery(const PropertyValuePtr &Qr,
                                const GuardedProgramValSet &Deps);

  bool operator==(const DirectNumericalQuery &R) const {
    return Q == R.Q && Eq == R.Eq;
  }

  size_t hash() const override {
    return gvfg_utility::hashHelper({QK, Q.hash(), (unsigned)Eq});
  }

  static bool classof(const NumericalQuery *V) {
    return V->getKind() == QK_DIRECT;
  }

  void dump() const override;

  SMTExprVec toSMT(PathCondSolver &Solver) const;

  GuardedProgramValSet getUsedVals() const override {
    GuardedProgramValSet Res = Deps;
    if (Q) {
      for (const auto &V : CastProperty<PropertySymExpr>(Q)->getUsedVars()) {
        Res.addValue(V.getValue());
      }
    }
    return Res;
  }

  PropertyValuePtr getSymExpr() const { return Q; }

  bool isEquation() const { return Eq; }

  bool mustSat() const { return !Q && getDeps().empty(); }

private:
  PropertyValuePtr Q; // default to nullptr
  bool Eq = false;
};

/// Query over a symbolic memory access.
///
/// Indirect queries keep the base pointer, symbolic offset, and access size
/// separate so the analysis can combine pointer facts with numerical reasoning
/// when approximating memory safety checks such as BOF.
class IndirectNumericalQuery : public NumericalQuery {
  IndirectNumericalQuery(unsigned BugTy, const ProgramValuePtr &BasePtr,
                         const PropertyValuePtr &Offset,
                         const PropertyValuePtr &AccSize,
                         const GuardedProgramValSet &Deps)
      : NumericalQuery(QK_INDIRECT, BugTy, Deps), BasePtr(BasePtr),
        Offset(Offset), AccSize(AccSize) {}

public:
  virtual ~IndirectNumericalQuery() {}

  static NumericalQueryPtr getBofQuery(const ProgramValuePtr &BasePtr,
                                       const PropertyValuePtr &Offset,
                                       const PropertyValuePtr &AccSize,
                                       const GuardedProgramValSet &Deps);

  bool operator==(const IndirectNumericalQuery &R) const {
    return BasePtr == R.BasePtr && Offset == R.Offset && AccSize == R.AccSize;
  }

  size_t hash() const override {
    return gvfg_utility::hashHelper(
        {QK, BasePtr.hash(), Offset.hash(), AccSize.hash()});
  }

  static bool classof(const NumericalQuery *O) {
    return O->getKind() == QK_INDIRECT;
  }

  void dump() const override;

  ProgramValuePtr getBasePtr() const { return BasePtr; }

  const PropertyValuePtr &getOffset() const { return Offset; }

  const PropertyValuePtr &getAccSize() const { return AccSize; }

  GuardedProgramValSet getUsedVals() const override {
    GuardedProgramValSet Res = Deps;
    if (IsaProperty<PropertySymExpr>(Offset)) {
      const auto &V1s = CastProperty<PropertySymExpr>(Offset)->getUsedVars();
      for (const auto &P : V1s) {
        Res.addValue(P.getValue());
      }
    }

    if (AccSize && IsaProperty<PropertySymExpr>(AccSize)) {
      const auto &V2s = CastProperty<PropertySymExpr>(AccSize)->getUsedVars();
      for (const auto &P : V2s) {
        Res.addValue(P.getValue());
      }
    }

    return Res;
  }

private:
  ProgramValuePtr BasePtr;
  PropertyValuePtr Offset;
  PropertyValuePtr AccSize;
};

} // namespace SymbolicExecution

#endif
