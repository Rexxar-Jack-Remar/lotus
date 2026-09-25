/** @file AnalysisState.h @brief Abstract state representation for symbolic execution. */
#pragma once

#include "llvm/IR/Operator.h"

#include "SymbolicExecution/Core/AnalysisLimit.h"
#include "SymbolicExecution/Core/CStringState.h"
#include "SymbolicExecution/Core/GuardedValue.h"
#include "SymbolicExecution/Core/ProgramVar.h"
#include "SymbolicExecution/Core/PropertyValue.h"
#include "SymbolicExecution/Core/Query.h"
#include "SymbolicExecution/Core/SymbolicMemory.h"
#include "SymbolicExecution/Core/TaintState.h"
#include "SymbolicExecution/Core/Trace.h"
#include "SymbolicExecution/Integration/GVFGUtility.h"
#include "SymbolicExecution/Solver/ConstraintRepr.h"
#include "SymbolicExecution/Solver/PathCondSolver.h"

#include <mutex>
#include <tuple>

class TaintModel;

namespace SymbolicExecution {
using lotus::gvfg::GuardedValueFlowCallOutputNode;
using lotus::gvfg::GuardedValueFlowCallSite;
using lotus::gvfg::GuardedValueFlowGraph;
using lotus::gvfg::GuardedValueFlowNode;
using lotus::gvfg::GuardedValueFlowPhiNode;
using lotus::gvfg::GuardedValueFlowRegionNode;
using lotus::gvfg::GuardedValueFlowReturnNode;
class AnalysisDriver;
class AnalysisSummary;

/// Symbolic state maintained while analyzing one function.
///
/// AnalysisState is the core mutable abstraction of the subsystem. It tracks
/// symbolic register values, points to information over symbolic access paths,
/// path conditions, taint propagation, freed memory facts, and call mapping
/// state used to instantiate summaries. When a function finishes, the relevant
/// portion of this state is packaged into an AnalysisSummary for reuse by later
/// callers.
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

