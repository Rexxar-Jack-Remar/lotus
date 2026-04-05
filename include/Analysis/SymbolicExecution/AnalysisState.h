#ifndef ANALYSIS_SYMBOLICEXECUTION_ANALYSISSTATE_H
#define ANALYSIS_SYMBOLICEXECUTION_ANALYSISSTATE_H

#include "llvm/IR/Operator.h"

#include "Analysis/SymbolicExecution/AnalysisLimit.h"
#include "Analysis/SymbolicExecution/ConstraintRepr.h"
#include "Analysis/SymbolicExecution/PathCondSolver.h"
#include "Analysis/SymbolicExecution/ProgramVar.h"
#include "Analysis/SymbolicExecution/PropertyValue.h"
#include "Analysis/SymbolicExecution/SegUtility.h"

#include <mutex>
#include <tuple>

namespace SymbolicExecution {
using lotus::gvfg::GuardedValueFlowCallOutputNode;
using lotus::gvfg::GuardedValueFlowCallSite;
using lotus::gvfg::GuardedValueFlowGraph;
using lotus::gvfg::GuardedValueFlowNode;
using lotus::gvfg::GuardedValueFlowPhiNode;
using lotus::gvfg::GuardedValueFlowRegionNode;
using lotus::gvfg::GuardedValueFlowReturnNode;
class AccessPath;
class PTItem;
class AnalysisDriver;
} // namespace SymbolicExecution

class TaintModel;

namespace std {
template <> struct hash<SymbolicExecution::Condition> {
  size_t operator()(const SymbolicExecution::Condition &V) const;
};

template <> struct hash<SymbolicExecution::AccessPath> {
  size_t operator()(const SymbolicExecution::AccessPath &V) const;
};

template <> struct hash<SymbolicExecution::PTItem> {
  size_t operator()(const SymbolicExecution::PTItem &V) const;
};
} // namespace std

namespace SymbolicExecution {
class PTItem;

/// Represents an abstract memory location: Base + Offset
class AccessPath {
  friend class PTItem;

public:
  AccessPath(const AccessPath &) = default;

  AccessPath(ProgramValuePtr Parent, PropertyValuePtr Offset)
      : Parent(std::move(Parent)), Offset(std::move(Offset)) {}

  AccessPath() : AccessPath(ProgramValuePtr(), PropertyValuePtr()) {}

  AccessPath &operator=(const AccessPath &) = default;

private:
  ProgramValuePtr Parent;
  PropertyValuePtr Offset;

public:
  ProgramValuePtr getParent() const { return Parent; }

  const PropertyValuePtr &getOffset() const { return Offset; }

  bool operator==(const AccessPath &R) const {
    return Parent == R.Parent && Offset == R.Offset;
  }

  bool operator<(const AccessPath &R) const {
    if (!(Parent == R.Parent))
      return Parent < R.Parent;
    return Offset.get() < R.Offset.get();
  }

  bool isEmpty() const {
    if (Parent == nullptr) {
      assert(!Offset);
      return true;
    } else {
      return false;
    }
  }

  size_t hash() const {
    return seg_utility::hashHelper({Parent.hash(), Offset.hash()});
  }

  std::string getID() const {
    if (isEmpty()) {
      return "0";
    } else {
      return Parent.getID() + "_" + std::to_string(Offset->hash());
    }
  }
};

/// Points-to set item. Represents a memory object with size and kind.
class PTItem {
public:
  enum MemObjKind { MK_CONCRETE, MK_SYMBOLIC, MK_PLACEHOLDER };

  PTItem(ProgramValuePtr AllocSite, MemObjKind K, int64_t Off = 0,
         const PropertyValuePtr &Sz = PropertyValuePtr());

  PTItem(AccessPath AP, MemObjKind K) : AP(std::move(AP)), Size(), K(K) {}

  bool operator==(const PTItem &R) const {
    return AP == R.AP && Size == R.Size && K == R.K;
  }

  size_t hash() const {
    return seg_utility::hashHelper({AP.hash(), Size.hash(), (unsigned)K});
  }

  bool isSymbolic() const;

  bool isConcrete() const;

  bool isPlaceHolder() const;

  bool isOffsetSymbolic() const;

  BigInteger getConstOffset() const;

  ProgramValuePtr getAllocSite() const { return AP.getParent(); }

  PropertyValuePtr getOffset() const { return AP.getOffset(); }

  PropertyValuePtr getSize() const { return Size; }

  AccessPath getAP() const { return AP; }

  PTItem offsetBy(const PropertyValuePtr &Off) const;

  PTItem offsetBy(int64_t Off) const;

  void changeOffsetBy(const PropertyValuePtr &Off);

  std::string getID() const;

private:
  AccessPath AP;
  PropertyValuePtr Size;
  MemObjKind K;
};

class GuardedSymbolicValSet;

/// A generic map from KeyTy to PathCondition, enforcing a size limit.
template <typename Derived, typename KeyTy,
          unsigned *Threshold = &AnalysisLimit::VALUE_SET_LIMIT_V>
class GuardedSet {
public:
  GuardedSet() = default;

  template <typename DestTy> DestTy convertTo() {
    DestTy Res;
    for (const auto &P : Vals) {
      if (Res.isFull()) {
        break;
      }
      Res.setKeyCond(P.first, P.second);
    }
    return Res;
  }

  void setKeyCond(const KeyTy &K, const Condition &Cond) { Vals[K] = Cond; }

  Derived merge(const Derived &R) const {
    Derived Res(static_cast<const Derived &>(*this));
    Res.addValues(R);
    return Res;
  }

  void translate(PathCondSolver *NewSolver) {
    for (auto &P : Vals) {
      P.second = P.second.translateNoLock(NewSolver);
    }
  }

  bool isFull() const { return Vals.size() >= *Threshold; }

  static unsigned getThreshold() { return *Threshold; }

  bool addValue(const KeyTy &Val, const Condition &Cond = Condition()) {
    if (Cond.isFalse() || isFull()) {
      return false;
    }

    if (Vals.count(Val)) {
      Condition OldCond = Vals.at(Val);
      Condition NewCond = OldCond || Cond;
      if (NewCond != OldCond) {
        setKeyCond(Val, NewCond);
      }

      return false;
    } else {
      setKeyCond(Val, Cond);
      return true;
    }
  }

  template <typename OtherTy>
  void addValues(const OtherTy &R, const Condition &Cond = Condition()) {
    if (Cond.isFalse() || isFull()) {
      return;
    }

    for (const auto &ValCond : R) {
      addValue(ValCond.first, ValCond.second && Cond);
    }
  }

  void setValues(const Derived R) {
    static_cast<Derived &>(*this) = std::move(R);
  }

  bool hasValue(const KeyTy &Val) const { return Vals.count(Val); }

  Condition getGuardForValue(const KeyTy &Val) const { return Vals.at(Val); }

  Derived operator&&(const Condition &Cond) const {
    Derived Res;

    for (const auto &P : Vals) {
      Condition C = P.second && Cond;
      if (!C.isFalse()) {
        Res.setKeyCond(P.first, C);
      }
    }

    return Res;
  }

  bool empty() const { return Vals.empty(); }

  size_t size() const { return Vals.size(); }

  typename std::unordered_map<KeyTy, Condition>::iterator begin() {
    return Vals.begin();
  }

  typename std::unordered_map<KeyTy, Condition>::iterator end() {
    return Vals.end();
  }

  typename std::unordered_map<KeyTy, Condition>::const_iterator begin() const {
    return Vals.begin();
  }

  typename std::unordered_map<KeyTy, Condition>::const_iterator end() const {
    return Vals.end();
  }

  void clear() { Vals.clear(); }

  bool isSingleton() const { return Vals.size() == 1; }

  const KeyTy &getAsSingleton() const {
    assert(isSingleton());
    return Vals.begin()->first;
  }

  const std::unordered_map<KeyTy, Condition> &getVals() const { return Vals; }

  using IterType =
      typename std::unordered_map<KeyTy, Condition>::const_iterator;

  using ElemType = KeyTy;

  void forEach(
      std::function<void(const KeyTy &K, const Condition &Cond)> Proc,
      std::function<bool()> StopCond = []() { return false; }) const {
    for (const auto &P : *this) {
      if (StopCond()) {
        return;
      }
      Proc(P.first, P.second);
    }
  }

  template <typename OtherTy>
  void forEach2(
      const OtherTy &Other,
      std::function<void(const KeyTy &K1, const typename OtherTy::ElemType &K2,
                         const Condition &C)>
          Proc,
      std::function<bool()> StopCond = []() { return false; }) const {
    for (const auto &P1 : *this) {
      for (const auto &P2 : Other) {
        Proc(P1.first, P2.first, P1.second && P2.second);
        if (StopCond()) {
          return;
        }
      }
    }
  }

  template <typename OtherTy, typename ResTy>
  std::vector<std::pair<ResTy, Condition>>
  zip2(const OtherTy &Other,
       std::function<ResTy(const KeyTy &K1,
                           const typename OtherTy::ElemType &K2)>
           Proc) const {
    std::vector<std::pair<ResTy, Condition>> Result;
    for (const auto &P1 : *this) {
      for (const auto &P2 : Other) {
        Result.emplace_back(
            std::make_pair(Proc(P1.first, P2.first), P1.second && P2.second));
      }
    }
    return Result;
  }

  template <typename OtherTy>
  OtherTy
  map(std::function<typename OtherTy::ElemType(const KeyTy &K1)> Proc) const {
    OtherTy Res;
    for (const auto &P : *this) {
      Res.addValue(Proc(P.first), P.second);
    }
    return Res;
  }

  template <typename ResTy>
  ResTy reduce(
      std::function<void(const KeyTy &, const Condition &, ResTy &)> Proc,
      std::function<bool(const ResTy &)> StopCond = [](const ResTy &) {
        return false;
      }) const {
    ResTy Res;
    for (const auto &P : *this) {
      Proc(P.first, P.second, Res);
      if (StopCond(Res)) {
        return Res;
      }
    }
    return Res;
  }

protected:
  std::unordered_map<KeyTy, Condition> Vals;
};

class PtsSet
    : public GuardedSet<PtsSet, PTItem, &AnalysisLimit::POINTS_SET_LIMIT_V> {
public:
  PtsSet offsetBy(const GuardedSymbolicValSet &Offs) const;
};

/// Tracks a set of symbolic values with path conditions.
class GuardedSymbolicValSet
    : public GuardedSet<GuardedSymbolicValSet, PropertyValuePtr,
                        &AnalysisLimit::SYMBOLIC_VAL_SET_LIMIT_V> {
public:
  static GuardedSymbolicValSet
  seqAdd(std::vector<std::tuple<GuardedSymbolicValSet::IterType,
                                GuardedSymbolicValSet::IterType, BigInteger,
                                ProgramValuePtr>>
             Seq,
         PathCondSolver *Solver, bool AddEqCond, const std::string &CSSuffix);

  GuardedSymbolicValSet() = default;
  GuardedSymbolicValSet(const PropertyValuePtr &V,
                        const Condition &Cond = Condition());

  GuardedSymbolicValSet
  binOp(const GuardedSymbolicValSet &Rhs, PropertyValue::BinOp Op,
        size_t Threshold = AnalysisLimit::SYMBOLIC_VAL_SET_LIMIT_V) const;

  GuardedSymbolicValSet
  binOp(const PropertyValue &R, PropertyValue::BinOp Op,
        size_t Threshold = AnalysisLimit::SYMBOLIC_VAL_SET_LIMIT_V) const;

  GuardedSymbolicValSet operator+(const GuardedSymbolicValSet &Rhs) const {
    return binOp(Rhs, PropertyValue::Add);
  }

  GuardedSymbolicValSet operator-(const GuardedSymbolicValSet &Rhs) const {
    return binOp(Rhs, PropertyValue::Sub);
  }

  GuardedSymbolicValSet operator*(const GuardedSymbolicValSet &Rhs) const {
    return binOp(Rhs, PropertyValue::Mul);
  }

  GuardedSymbolicValSet operator+(const PropertyValue &Rhs) const {
    return binOp(Rhs, PropertyValue::Add);
  }

  GuardedSymbolicValSet operator-(const PropertyValue &Rhs) const {
    return binOp(Rhs, PropertyValue::Sub);
  }

  GuardedSymbolicValSet operator*(const PropertyValue &Rhs) const {
    return binOp(Rhs, PropertyValue::Mul);
  }

  std::vector<Condition> cmp(const GuardedSymbolicValSet &Rhs,
                             unsigned Pred) const;
};

class GuardedProgramValSet
    : public GuardedSet<GuardedProgramValSet, ProgramValuePtr> {
public:
  GuardedProgramValSet() = default;
  GuardedProgramValSet(const ProgramValuePtr &V) { addValue(V); }
};
class GuardedAccessPathSet
    : public GuardedSet<GuardedAccessPathSet, AccessPath> {};

class AnalysisState;

/// Manages string length tracking for C strings.
class CStringState {
  friend class AnalysisSummary;

public:
  static ProgramValuePtr getLenVAtCaller(const ProgramValuePtr &LenV,
                                         Instruction *CS);

  void handleCStrLen(const ProgramValuePtr &Ptr, Instruction *Loc,
                     const Condition &Cond, AnalysisState &CurState,
                     bool IsDirect);

  GuardedSymbolicValSet getCStrlen(const ProgramValuePtr &Ptr, Instruction *Loc,
                                   bool IsDirect,
                                   const AnalysisState &CurState) const;

  void onProcessCall(Instruction *Inst, Function *Callee,
                     AnalysisState &CurState, const CStringState &Smry);

  const std::unordered_map<ProgramValuePtr, PtsSet> &getLenPts() const {
    return LenPts;
  }

private:
  std::unordered_map<ProgramValuePtr, PtsSet> LenPts;

  ProgramValuePtr getLenVariable(const ProgramValuePtr &Ptr, Instruction *Loc,
                                 const AnalysisState &CurState,
                                 bool IsDirect) const;

  GuardedSymbolicValSet computeCStrLength(const ProgramValuePtr &Len,
                                          Instruction *Loc, const PtsSet &Pts,
                                          AnalysisState &CurState,
                                          const Condition &Cond);

  GuardedSymbolicValSet computeCStrLength(const ProgramValuePtr &Len,
                                          Instruction *Loc, const PTItem &Pt,
                                          AnalysisState &CurState,
                                          const Condition &Cond);

  void translate(PathCondSolver *NewSolver) {
    for (auto &P : LenPts) {
      P.second.translate(NewSolver);
    }
  }
};

class TaintStep;

class TaintValSet : public GuardedSet<TaintValSet, ProgramValuePtr,
                                      &AnalysisLimit::TAINT_VAL_SET_LIMIT_V> {};
/// Summary of taint information for a function.
class TaintSummary {
  friend class AnalysisState;
  friend class AnalysisSummary;

public:
  TaintSummary(const TaintValSet &TaintedFormals,
               const TaintValSet &TaintedRets,
               const std::unordered_map<ProgramValuePtr, std::vector<TaintStep>>
                   &TaintedSteps)
      : TaintedFormals(TaintedFormals), TaintedRets(TaintedRets),
        TaintedSteps(TaintedSteps) {}

  TaintSummary() {}

private:
  TaintValSet TaintedFormals;
  TaintValSet TaintedRets;
  std::unordered_map<ProgramValuePtr, std::vector<TaintStep>> TaintedSteps;

  void translate(PathCondSolver *NewSolver) {
    TaintedFormals.translate(NewSolver);
    TaintedRets.translate(NewSolver);
  }

public:
  std::vector<TaintStep> getTaintSteps(const ProgramValuePtr &V) const {
    return TaintedSteps.at(V);
  }
};

class NumericalQueryPtr;
class QuerySet;

/// Abstract base class for numerical bug queries (BOF, DBZ, etc.).
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

/// A direct query involves checking a specific condition or value.
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
    return seg_utility::hashHelper({QK, Q.hash(), (unsigned)Eq});
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

/// An indirect query involves base pointer, offset, and access size.
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
    return seg_utility::hashHelper(
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

class NumericalQueryPtr {
  friend class AnalysisSummary;

public:
  NumericalQueryPtr() {}
  NumericalQueryPtr(const std::shared_ptr<NumericalQuery> &V) : Data(V) {}
  NumericalQueryPtr(const std::shared_ptr<DirectNumericalQuery> &V) : Data(V) {}
  NumericalQueryPtr(const std::shared_ptr<IndirectNumericalQuery> &V)
      : Data(V) {}
  NumericalQueryPtr(const NumericalQueryPtr &) = default;
  NumericalQueryPtr &operator=(const NumericalQueryPtr &) = default;

  bool operator==(const NumericalQueryPtr &R) const {
    if (Data == R.Data) {
      return true;
    }

    auto Op1 = get(), Op2 = R.get();
    if (Op1->getKind() != Op2->getKind() ||
        Op1->getBugTy() != Op2->getBugTy()) {
      return false;
    }

    if (isa<DirectNumericalQuery>(Op1)) {
      return *cast<DirectNumericalQuery>(Op1) ==
             *cast<DirectNumericalQuery>(Op2);
    } else {
      return *cast<IndirectNumericalQuery>(Op1) ==
             *cast<IndirectNumericalQuery>(Op2);
    }
  }

  size_t hash() const { return Data->hash(); }

  const NumericalQuery *operator->() const { return get(); }

  operator bool() const { return Data != nullptr; }

  NumericalQuery *get() const { return Data.get(); }

private:
  std::shared_ptr<NumericalQuery> Data;

  void translate(PathCondSolver *Solver) const {
    if (Data) {
      Data->translate(Solver);
    }
  }
};

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

class TraceStep;
/// Captures the analysis results for a function to be used as a summary.
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

class TraceStep {
public:
  enum TraceStepKind {
    TRACE_STEP_CALL,
    TRACE_STEP_ALLOC,
    TRACE_STEP_BUFFER_ACCESS,
    TRACE_STEP_DIV,
    TRACE_STEP_ARITH,
    TRACE_STEP_DEREF
  };

  TraceStep(TraceStepKind TK, Instruction *Inst, Value *Val)
      : TK(TK), Inst(Inst), Val(Val) {}

  TraceStepKind TK;
  Instruction *Inst;
  Value *Val;
};

class TaintStep {
public:
  enum TaintStepKind { TAINT_STEP_SOURCE, TAINT_STEP_CALL, TAINT_STEP_PROP };

  TaintStep(TaintStepKind TK, Instruction *Inst, Value *V1 = nullptr,
            Value *V2 = nullptr)
      : TK(TK), Inst(Inst), V1(V1), V2(V2) {}

  TaintStepKind TK;
  Instruction *Inst;
  // TAINT_STEP_SOURCE: V1 = taint source
  // TAINT_STEP_Prop: (V1, V2) = (Src val, Dst val)
  Value *V1;
  Value *V2;
};

/// The main state class for symbolic execution within a function.
/// Manages registers (symbolic values), memory (points-to graph), constraints,
/// and taint info.
class AnalysisState {
  friend class AnalysisSummary;
  friend class CStringState;
  friend class SummarySolverManager;
  friend class AbsStore;
  friend class RegionCondition;

public:
  enum SymexBugType {
    BUG_TY_UNDEF = 0x0,
    BUG_TY_BOF = 0x01,
    BUG_TY_DBZ = 0x10,
    BUG_TY_INT_OVERFLOW = 0x100,
    BUG_TY_INT_UNDERFLOW = 0x200,
    BUG_TY_NULL_DEREF = 0x400,
    BUG_TY_SIGNED_INT_OVERFLOW = 0x800,
    BUG_TY_SIGNED_INT_UNDERFLOW = 0x1000,
    BUG_TY_SHIFT_OVERFLOW = 0x2000,
    BUG_TY_ARRAY_INDEX_OOB = 0x4000,
    BUG_TY_UNINIT_READ = 0x8000,
    BUG_TY_UAF = 0x10000,
    BUG_TY_DOUBLE_FREE = 0x20000,
    BUG_TY_NEGATIVE_ARRAY_INDEX = 0x40000,
    BUG_TY_INT_TRUNCATION = 0x80000,
  };

  AnalysisState(SymexBugType BugTy, GuardedValueFlowGraph *Graph,
                Function *Func);

  void transfer(Instruction *Inst, AnalysisDriver &Driver);

  void finalizeSummary();

  static Type *NON_PTR_TY;
  static Type *INT8_TY;

  const std::vector<std::tuple<SymexBugType, std::vector<TaintStep>,
                               std::vector<TraceStep>>> &
  getBugReports() const {
    return BugReports;
  }

  bool hasSymbolicVals(const ProgramValuePtr &V) const { return Regs.count(V); }

  const GuardedSymbolicValSet &getSymbolicVals(const ProgramValuePtr &V) const {
    return Regs.at(V);
  }

  PathCondSolver *getSolver() const { return Solver.get(); }

  GuardedValueFlowGraph *getGraph() const { return Graph; }

private:
  GuardedValueFlowNode *getNode(Value *V) const;

  class RenameCtx {
  public:
    RenameCtx(const SMTExpr &FormalExpr) : FormalExpr(FormalExpr) {}

    void add(Instruction *CS, const SMTExpr &RealExpr) {
      CSToRealExpr.insert(std::make_pair(CS, RealExpr));
    }

    SMTExpr getFormalExpr() const { return FormalExpr; }

    bool hasRealExpr(Instruction *CS) const { return CSToRealExpr.count(CS); }

    SMTExpr getRealExpr(Instruction *CS) const { return CSToRealExpr.at(CS); }

  private:
    SMTExpr FormalExpr;
    std::unordered_map<Instruction *, SMTExpr> CSToRealExpr;
  };

  SymexBugType BugTy = BUG_TY_UNDEF;
  GuardedValueFlowGraph *Graph = nullptr;
  Function *F = nullptr;
  TaintModel *TaintSpec = nullptr;
  std::unique_ptr<PathCondSolver> Solver = nullptr;
  ReturnInst *RetI = nullptr;
  mutable size_t FreeVarID = 0;

  // Current state
  std::unordered_map<ProgramValuePtr, PtsSet> PointsTo;
  std::unordered_map<ProgramValuePtr, GuardedSymbolicValSet> Regs;
  std::unordered_set<ProgramValuePtr> UnknownSyms;

  std::unordered_map<ProgramValuePtr, GuardedSymbolicValSet> EscapeAllocToSizes;

  mutable std::unordered_map<BasicBlock *, Condition> LocalCondMap;
  mutable std::unordered_map<GuardedValueFlowRegionNode *, Condition>
      RegionCondMap;
  mutable std::unordered_map<const GuardedValueFlowCallOutputNode *, Condition>
      CSOutputCondMap;
  mutable std::unordered_map<GuardedValueFlowNode *, Condition> DataDepsCondMap;

  // Summary
  TaintSummary TaintSmry;
  std::unordered_map<ProgramValuePtr, PtsSet> OutputPts;
  std::unordered_map<ProgramValuePtr, GuardedSymbolicValSet> OutSymbolicValMap;
  std::unordered_map<Instruction *,
                     std::unordered_map<ProgramValuePtr, ProgramValuePtr>>
      FormalToRealMap;

  // FormalToRealMap ==  FormalToRealArgMap ∪ FormalToRealEscapeMap ∪
  // FormalToRealLenMap
  std::unordered_map<Instruction *,
                     std::unordered_map<ProgramValuePtr, ProgramValuePtr>>
      FormalToRealArgMap;

  std::unordered_map<Instruction *,
                     std::unordered_map<ProgramValuePtr, ProgramValuePtr>>
      FormalToRealEscapeMap;

  std::unordered_map<Instruction *,
                     std::unordered_map<ProgramValuePtr, ProgramValuePtr>>
      FormalToRealLenMap;

  // cs -> formal -> symbolic values passed in to the real associated with
  // formal
  std::unordered_map<Instruction *,
                     std::unordered_map<ProgramValuePtr, GuardedSymbolicValSet>>
      FormalToRealSymValsMap;

  // func -> formal of func -> cs of func -> (formal_expr, real_expr)
  std::unordered_map<Function *, std::unordered_map<ProgramValuePtr, RenameCtx>>
      SMTRenameCtxMap;

  // Sub state
  TaintValSet TaintedVals;
  std::unordered_map<ProgramValuePtr, std::vector<TaintStep>> TaintedSteps;
  CStringState StrState;

  // Freed memory tracking for UAF and Double-Free detection
  // Maps freed pointer to the condition under which it was freed
  std::unordered_map<ProgramValuePtr, Condition> FreedPointers;
  // Tracks pointers that have been freed (for double-free detection)
  GuardedProgramValSet FreedPtrSet;

  // (query, a set of instructions of the form call -> ...-> sink)
  // Notice that vector[0] is the sink instruction
  std::vector<std::pair<QuerySet, std::vector<TraceStep>>> QueryToTraces;

  std::unordered_map<ProgramValuePtr, GuardedProgramValSet> ExtraDepsMap;
  // set of (bug-type, taint-steps, query-steps)
  std::vector<
      std::tuple<SymexBugType, std::vector<TaintStep>, std::vector<TraceStep>>>
      BugReports;
  unsigned QueryCount = 0;

  // Caches
  mutable std::unordered_map<
      PropertyValuePtr,
      std::unordered_map<Instruction *, GuardedSymbolicValSet>>
      InlineExprCache;
  mutable std::map<NumericalQuery *,
                   std::unordered_map<Instruction *, QuerySet>>
      InlineQueryCache;
  // call site -> callee condition index -> inlined caller cond (not considering
  // function pointers)
  mutable std::unordered_map<Instruction *,
                             std::unordered_map<int64_t, Condition>>
      InlineCondCache;
  mutable std::unordered_map<Instruction *, Condition> MappingCondCache;

  // statement transformers
  void processBinaryInst(Instruction *Inst);
  void processICmpInst(Instruction *Inst);
  void processAlloca(Instruction *Inst);
  void processPhiInst(Instruction *Inst);
  void processGEP(Instruction *Inst);
  void processLoad(Instruction *Inst);
  void collectEscapeObjs();
  void processReturn();

  void processLoadPtr(const ProgramValuePtr &Ptr,
                      const GuardedValueFlowNode *LdMemNode,
                      const ProgramValuePtr &Dst, Instruction *Pos);
  void processStore(Instruction *Inst);
  void processCall(CallInst *Inst, Function *Callee,
                   const AnalysisSummary &Smry);
  void processLibraryCall(CallInst *Inst);
  void processAsUnknownLib(CallInst *Inst);
  void processFreeCall(CallInst *Inst);

  // FIXME: patch to falcon
  // void processMemcpy(Instruction *Pos, Value *Dst, Value *Src, int64_t Len,
  //                   const Condition &PreCond);
  // bool processConstStrcpy(Instruction *Pos, Value *Dst, Value *Src, int64_t
  // Len,
  //                        const Condition &PreCond);
  // void processMemset(Instruction *Pos, Value *Dst, Value *Val, int64_t Len,
  //                   const Condition &PreCond);

  void taintInit(Function *Func);

  void taintTransfer(Instruction *Inst);
  void taintProcessCall(CallInst *Inst, Function *Callee,
                        const TaintSummary &Smry);
  void buildTaintSummary();

  void buildQuery(Instruction *Inst);

  void buildBofQueryLoadStore(Instruction *Inst, const ProgramValuePtr &Ptr,
                              Type *AccTy);
  void buildBofQueryLibCall(CallInst *Inst);

  void buildDbzQuery(Instruction *Inst);
  void buildIntOverflowQuery(Instruction *Inst);
  void buildIntUnderflowQuery(Instruction *Inst);
  void buildNullDerefQuery(Instruction *Inst);
  void buildSignedIntOverflowQuery(Instruction *Inst);
  void buildSignedIntUnderflowQuery(Instruction *Inst);
  void buildShiftOverflowQuery(Instruction *Inst);
  void buildArrayIndexOOBQuery(Instruction *Inst);
  void buildUninitializedReadQuery(Instruction *Inst);
  void buildUafQuery(Instruction *Inst);
  void buildDoubleFreeQuery(Instruction *Inst);
  void buildNegativeArrayIndexQuery(Instruction *Inst);
  void buildIntTruncationQuery(Instruction *Inst);

  bool tryReportBofQuery(const NumericalQueryPtr &Q, const Condition &Cond,
                         Function *SinkFun,
                         const std::vector<TraceStep> &Trace);

  bool tryReportDbzQuery(const NumericalQueryPtr &Q, const Condition &Cond,
                         Function *SinkFun,
                         const std::vector<TraceStep> &Trace);

  bool tryReportIntOverflowQuery(const NumericalQueryPtr &Q,
                                 const Condition &Cond, Function *SinkFun,
                                 const std::vector<TraceStep> &Trace);
  bool tryReportIntUnderflowQuery(const NumericalQueryPtr &Q,
                                  const Condition &Cond, Function *SinkFun,
                                  const std::vector<TraceStep> &Trace);
  bool tryReportNullDerefQuery(const NumericalQueryPtr &Q,
                               const Condition &Cond, Function *SinkFun,
                               const std::vector<TraceStep> &Trace);
  bool tryReportSignedIntOverflowQuery(const NumericalQueryPtr &Q,
                                       const Condition &Cond, Function *SinkFun,
                                       const std::vector<TraceStep> &Trace);
  bool tryReportSignedIntUnderflowQuery(const NumericalQueryPtr &Q,
                                        const Condition &Cond,
                                        Function *SinkFun,
                                        const std::vector<TraceStep> &Trace);
  bool tryReportShiftOverflowQuery(const NumericalQueryPtr &Q,
                                   const Condition &Cond, Function *SinkFun,
                                   const std::vector<TraceStep> &Trace);
  bool tryReportArrayIndexOOBQuery(const NumericalQueryPtr &Q,
                                   const Condition &Cond, Function *SinkFun,
                                   const std::vector<TraceStep> &Trace);
  bool tryReportUninitializedReadQuery(const NumericalQueryPtr &Q,
                                       const Condition &Cond, Function *SinkFun,
                                       const std::vector<TraceStep> &Trace);
  bool tryReportUafQuery(const NumericalQueryPtr &Q, const Condition &Cond,
                         Function *SinkFun,
                         const std::vector<TraceStep> &Trace);
  bool tryReportDoubleFreeQuery(const NumericalQueryPtr &Q,
                                const Condition &Cond, Function *SinkFun,
                                const std::vector<TraceStep> &Trace);
  bool tryReportNegativeArrayIndexQuery(const NumericalQueryPtr &Q,
                                        const Condition &Cond,
                                        Function *SinkFun,
                                        const std::vector<TraceStep> &Trace);
  bool tryReportIntTruncationQuery(const NumericalQueryPtr &Q,
                                   const Condition &Cond, Function *SinkFun,
                                   const std::vector<TraceStep> &Trace);

  void buildQuerySummary();

  void queryProcessCall(
      Instruction *Inst, Function *Callee,
      const std::vector<std::pair<QuerySet, std::vector<TraceStep>>>
          &QueryToTraces);

  void initializeFormalToRealMap(CallInst *Inst, const AnalysisSummary &Smry);

  GuardedSymbolicValSet inlineVals(const GuardedSymbolicValSet &Vals,
                                   Instruction *CS, Function *Callee,
                                   const Condition &CSCond) const;

  PropertyValuePtr getAbsurdStrLen() const;

  bool mustBeConstantInt(const ProgramValuePtr &V) const;

  std::pair<PtsSet, bool> inlineVals(const PtsSet &Vals, Instruction *CS,
                                     Function *Callee,
                                     const Condition &CSCond) const;

  QuerySet inlineVals(const QuerySet &Vals, Instruction *CS,
                      Function *Callee) const;
  GuardedProgramValSet inlineVals(const GuardedProgramValSet &Vals,
                                  Instruction *CS, Function *Callee) const;

  // Implementation helpers
  void setPts(const ProgramValuePtr &Dst, const PtsSet &Pts);
  void assignVal(const ProgramValuePtr &Dst, const ProgramValuePtr &Src,
                 const Condition &Cond = Condition());
  void assignVals(const ProgramValuePtr &Dst, const GuardedProgramValSet &Src);
  void assignPtr(const ProgramValuePtr &Dst, const ProgramValuePtr &Src,
                 const GuardedSymbolicValSet &Offset, bool StrongUpdate,
                 const Condition &Cond = Condition());

  std::string getCallsiteSuffix(Instruction *CS) const;

  Condition getMappingCond(Instruction *CS, Function *Callee) const;

  SMTExpr buildVarEqValues(const ProgramValuePtr &V, SMTExpr VExpr,
                           const GuardedSymbolicValSet &SymVals) const;

  Condition transCond(Instruction *CS, Function *Callee,
                      const Condition &CalleeCond) const;

  GuardedSymbolicValSet inlineExpr(const PropertyValuePtr &E,
                                   Instruction *CS) const;

  ProgramValuePtr getFreeVar(Type *Ty) const;

  /// Evaluate ``E'' under the environment ``M''.
  GuardedSymbolicValSet
  evalExpr(const PropertyValuePtr &E,
           const std::unordered_map<ProgramValuePtr, GuardedSymbolicValSet> &M,
           Instruction *CS = nullptr) const;

  void createMemoryObject(const ProgramValuePtr &Ptr, PTItem::MemObjKind Kind,
                          const PropertyValuePtr &Sz = PropertyValuePtr(),
                          const Condition &Cond = Condition());
  PropertyValuePtr computeOffsets(GEPOperator *GEP);
  void initPointsToTarget(const ProgramValuePtr &Ptr, Instruction *Pos);

  void processMemAlloc(CallInst *Inst,
                       const std::vector<unsigned> &AllocSizeArgs);
  void handleMalloc(CallInst *Inst, unsigned SzIdx);
  void handleCalloc(CallInst *Inst, unsigned NumIdx, unsigned SzIdx);

  // Implementing taint analysis
  void processCallTaintSources(Instruction *Inst);
  void processTaintPropagation(Instruction *Inst);

  void propagateTaintPointer(const ProgramValuePtr &LdPtr,
                             const ProgramValuePtr &LdVal, Instruction *Inst,
                             const Condition &Cond);

  void propagateTaint(const ProgramValuePtr &Src, const ProgramValuePtr &Dst,
                      Instruction *Inst, const Condition &Cond,
                      bool Peel = true);
  Condition getTaintedCond(const ProgramValuePtr &V) const;
  std::unordered_map<ProgramValuePtr, std::unordered_set<ProgramValuePtr>>
  getTaintTransferTargets(Instruction *Inst) const;
  void taintVal(const ProgramValuePtr &V, const std::vector<TaintStep> &Steps,
                const Condition &PreCond, bool Peel = true);
  void markTaint(const ProgramValuePtr &V, const std::vector<TaintStep> &Steps,
                 const Condition &Cond);

  // Implementing query
  void addExtraDeps(const ProgramValuePtr &Dst, const ProgramValuePtr &Src,
                    const Condition &Cond);
  GuardedProgramValSet getDepsVals(const PropertyValuePtr &V) const;

  std::vector<NumericalQueryPtr> createBofQuery(
      const PTItem &Pt, const PropertyValuePtr &AccSz,
      const GuardedProgramValSet &Deps = GuardedProgramValSet()) const;

  void addQueryTrace(const NumericalQueryPtr &Q, const Condition &Cond,
                     const std::vector<TraceStep> &Trace);

  void addQueryTrace(const NumericalQueryPtr &Q, const Condition &Cond,
                     const TraceStep &Step);

  QuerySet inlineBofQuery(const NumericalQueryPtr &Q, Instruction *Inst,
                          Function *Callee) const;

  QuerySet inlineDbzQuery(const NumericalQueryPtr &Q, Instruction *Inst,
                          Function *Callee) const;

  QuerySet inlineIntOverflowQuery(const NumericalQueryPtr &Q, Instruction *Inst,
                                  Function *Callee) const;
  QuerySet inlineIntUnderflowQuery(const NumericalQueryPtr &Q,
                                   Instruction *Inst, Function *Callee) const;
  QuerySet inlineNullDerefQuery(const NumericalQueryPtr &Q, Instruction *Inst,
                                Function *Callee) const;
  QuerySet inlineSignedIntOverflowQuery(const NumericalQueryPtr &Q,
                                        Instruction *Inst,
                                        Function *Callee) const;
  QuerySet inlineSignedIntUnderflowQuery(const NumericalQueryPtr &Q,
                                         Instruction *Inst,
                                         Function *Callee) const;
  QuerySet inlineShiftOverflowQuery(const NumericalQueryPtr &Q,
                                    Instruction *Inst, Function *Callee) const;
  QuerySet inlineArrayIndexOOBQuery(const NumericalQueryPtr &Q,
                                    Instruction *Inst, Function *Callee) const;
  QuerySet inlineUninitializedReadQuery(const NumericalQueryPtr &Q,
                                        Instruction *Inst,
                                        Function *Callee) const;
  QuerySet inlineUafQuery(const NumericalQueryPtr &Q, Instruction *Inst,
                          Function *Callee) const;
  QuerySet inlineDoubleFreeQuery(const NumericalQueryPtr &Q, Instruction *Inst,
                                 Function *Callee) const;
  QuerySet inlineNegativeArrayIndexQuery(const NumericalQueryPtr &Q,
                                         Instruction *Inst,
                                         Function *Callee) const;
  QuerySet inlineIntTruncationQuery(const NumericalQueryPtr &Q,
                                    Instruction *Inst, Function *Callee) const;

  QuerySet inlineQuery(const NumericalQueryPtr &Q, Instruction *Inst,
                       Function *Callee) const;

  void setSymbolicVals(const ProgramValuePtr &V,
                       const GuardedSymbolicValSet &Vals) {
    Regs[V] = Vals;
  }

  void addSymbolicVals(const ProgramValuePtr &V,
                       const GuardedSymbolicValSet &Vals) {
    Regs[V].addValues(Vals);
  }

  void addUnknownSym(const ProgramValuePtr &V) { UnknownSyms.insert(V); }

  void initSymbol(const ProgramValuePtr &V);

  bool hasFormal(Instruction *CS, const ProgramValuePtr &Formal) const;

  ProgramValuePtr getRealForFormal(Instruction *CS,
                                   const ProgramValuePtr &Formal) const;

  void addFormalToRealArgMap(GuardedValueFlowCallSite *GraphCS,
                             Function *Callee, const AnalysisSummary &Smry);

  void addFormalToRealLenMap(GuardedValueFlowCallSite *GraphCS,
                             Function *Callee, const AnalysisSummary &Smry);

  void addFormalToRealEscapeMap(GuardedValueFlowCallSite *GraphCS,
                                Function *Callee, const AnalysisSummary &Smry);

  void addFormalToRealValues(GuardedValueFlowCallSite *GraphCS,
                             Function *Callee, const ProgramValuePtr &Formal,
                             const ProgramValuePtr &Real);

  GuardedSymbolicValSet getStrlen(const ProgramValuePtr &Ptr,
                                  Instruction *Loc) const {
    return StrState.getCStrlen(Ptr, Loc, false, *this);
  }

  Condition getLocalCond(BasicBlock *BB) const;

  Condition getDataDepsCond(GuardedValueFlowNode *N) const;

  Condition getCallSiteOutDeps() const;

  Condition getRegionCond(GuardedValueFlowRegionNode *R) const;

  Condition getPhiCond(const GuardedValueFlowPhiNode *PhiNode,
                       const GuardedValueFlowPhiNode::Incoming InNode) const;

  bool hasPts(const ProgramValuePtr &Ptr) const { return PointsTo.count(Ptr); }

  const PtsSet &getPts(const ProgramValuePtr &Ptr) const {
    return PointsTo.at(Ptr);
  }

  bool isInstUnmodelled(Instruction *Inst) const;

  GuardedSymbolicValSet computeCStrLength(Instruction *Pos, const PTItem &Pt,
                                          const Condition &Cond);
};

bool isPseudoArgVal(const ProgramValuePtr &V);

} // namespace SymbolicExecution

namespace std {
template <> struct hash<SymbolicExecution::NumericalQueryPtr> {
  size_t operator()(const SymbolicExecution::NumericalQueryPtr &V) const {
    return V.hash();
  }
};
} // namespace std

namespace SymbolicExecution {
class QuerySet : public GuardedSet<QuerySet, NumericalQueryPtr,
                                   &AnalysisLimit::INST_QUERY_LIMIT_V> {
public:
  QuerySet() = default;
  QuerySet(const NumericalQueryPtr &Q) { addValue(Q); }
};
} // namespace SymbolicExecution

#endif
