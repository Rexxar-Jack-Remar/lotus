//===----------------------------------------------------------------------===//
//
// AnalysisState summary management.
// Handles function summaries, call processing (inlining), and solver management
// across functions.
//
//===----------------------------------------------------------------------===//

#include "llvm/IR/CFG.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"

#include "Analysis/SymbolicExecution/AnalysisDriver.h"
#include "Analysis/SymbolicExecution/AnalysisState.h"
#include "Analysis/SymbolicExecution/DomTreePass.h"
#include "Analysis/SymbolicExecution/InstResolver.h"
#include "Analysis/SymbolicExecution/MemoryAPI.h"
#include "Analysis/SymbolicExecution/PropertyAllocator.h"
#include "Analysis/SymbolicExecution/PropertyInteger.h"
#include "Analysis/SymbolicExecution/PropertySym.h"
#include "Analysis/SymbolicExecution/TaintModel.h"

#include <functional>
#include <numeric>

#define DEBUG_TYPE "Symex"

using namespace SymbolicExecution;

/// Finalize analysis state into a summary.
void AnalysisState::finalizeSummary() {
  // Summary finalization packages the parts of the current function state that
  // can be consumed interprocedurally. Return facts, taint facts, and pending
  // bug queries are all normalized here so callers can import one summary
  // object instead of replaying the callee analysis.
  processReturn();
  buildTaintSummary();
  buildQuerySummary();
}

/// Process a function call by inlining the callee's summary.
void AnalysisState::processCall(CallInst *Inst, Function *Callee,
                                const AnalysisSummary &Smry) {
  // Import is organized in phases. First we build formal-to-real mappings for
  // ordinary arguments, synthetic length values, and escape-related symbols.
  // Then we re-instantiate the callee summary into caller state: memory facts,
  // points-to facts, scalar values, taint information, and deferred bug
  // queries. Each phase uses the same renaming context so conditions and
  // symbolic expressions stay aligned at this call site.
  initializeFormalToRealMap(Inst, Smry);

  auto *GraphCS = Graph->findSite<GuardedValueFlowCallSite>(Inst);
  auto *CalleeGraph = Smry.getGraph();

  addFormalToRealArgMap(GraphCS, Callee, Smry);
  addFormalToRealLenMap(GraphCS, Callee, Smry);
  addFormalToRealEscapeMap(GraphCS, Callee, Smry);

  // Collect incoming memory-backed values for call inputs before output facts
  // are inlined. This preserves the load/store view that the callee summary
  // expects when pseudo inputs represent memory-visible values.
  for (auto Iter = GraphCS->input_begin(Callee),
            EIter = GraphCS->input_end(Callee);
       Iter != EIter; ++Iter) {
    auto *InputNode = (*Iter).InputNode;

    if ((*Iter).IsCommonInput) {
      initSymbol(InputNode);
      continue;
    }

    assert(isa<GuardedValueFlowCallOutputNode>(InputNode));

    assert(InputNode->getNumChildren() == 1);

    auto *LdMemNode = cast<GuardedValueFlowNode>(InputNode->getChild(0));
    // LdPtr is a value from callee
    ProgramValuePtr BasePtr;
    auto *LdPtr = cast<GuardedValueFlowCallOutputNode>(InputNode)
                      ->getAccessPath()
                      .get_base_ptr();

    auto *LdPtrNode = CalleeGraph->findNode(LdPtr);
    if (!LdPtrNode || !hasFormal(Inst, LdPtrNode)) {
    } else {
      BasePtr = getRealForFormal(Inst, LdPtrNode);
    }

    processLoadPtr(BasePtr, LdMemNode, InputNode, Inst);

    initSymbol(InputNode);
  }

  for (const auto &P : FormalToRealArgMap.at(Inst)) {
    addFormalToRealValues(GraphCS, Callee, P.first, P.second);
  }

  // Escape objects conceptually allocate caller-visible memory during summary
  // import. They must exist before output points-to facts are installed so any
  // returned or stored pointer can target the recreated object immediately.
  Condition CSCond = getLocalCond(Inst->getParent());
  const auto &EscapeAllocToSizes = Smry.getEscapeInfo();
  for (const auto &P : FormalToRealEscapeMap.at(Inst)) {
    auto Formal = P.first, Real = P.second;

    initSymbol(Real);
    addFormalToRealValues(GraphCS, Callee, Formal, Real);

    assert(EscapeAllocToSizes.count(Formal));
    auto AccSizes =
        inlineVals(EscapeAllocToSizes.at(Formal), Inst, Callee, CSCond);
    AccSizes.forEach([&](const PropertyValuePtr &Sz, const Condition &C) {
      createMemoryObject(Real, PTItem::MK_CONCRETE, Sz, C);
    });
  }

  const auto &OutputPts = Smry.OutputPts;
  const auto &RetSymbolicValMap = Smry.getOutSymbolicValMap();

  /// Inline the callee's points-to summary for returns and pseudo returns.
  /// This recreates the callee's output heap view in caller coordinates using
  /// the formal-to-real mapping and the call-site condition.
  for (auto Iter = CalleeGraph->return_begin(),
            EIter = CalleeGraph->return_end();
       Iter != EIter; ++Iter) {
    auto *RetNode = *Iter;
    const GuardedValueFlowNode *OutNode = nullptr;
    if (RetNode->getKind() == GuardedValueFlowNode::Kind::CommonReturn) {
      OutNode = getNode(Inst);
    } else {
      OutNode = GraphCS->getPseudoOutput(
          Callee, cast<GuardedValueFlowReturnNode>(RetNode)->getIndex());
    }

    if (OutputPts.count(RetNode)) {
      const auto &RetPts = OutputPts.at(RetNode);
      auto InlinedPts = inlineVals(RetPts, Inst, Callee, CSCond).first;
      setPts(OutNode, InlinedPts);
    }
  }

  /// Rebuild derived string-length facts after pointer outputs are available,
  /// because length summaries may refer to the same imported buffers.
  StrState.onProcessCall(Inst, Callee, *this, Smry.StrState);
  for (const auto &P : FormalToRealLenMap.at(Inst)) {
    addFormalToRealValues(GraphCS, Callee, P.first, P.second);
  }

  /// Re-instantiate scalar summary facts for return nodes. If a summary carries
  /// no concrete symbolic alternatives, we still seed the receiver with its own
  /// symbolic variable so downstream transfer functions can keep referring to
  /// the returned value.
  for (auto Iter = CalleeGraph->return_begin(),
            EIter = CalleeGraph->return_end();
       Iter != EIter; ++Iter) {
    auto *RetNode = *Iter;
    const GuardedValueFlowNode *OutNode = nullptr;
    if (RetNode->getKind() == GuardedValueFlowNode::Kind::CommonReturn) {
      OutNode = getNode(Inst);
    } else {
      OutNode = GraphCS->getPseudoOutput(
          Callee, cast<GuardedValueFlowReturnNode>(RetNode)->getIndex());
    }

    const auto &RetVals = RetSymbolicValMap.at(RetNode);
    GuardedSymbolicValSet InlinedVals =
        inlineVals(RetVals, Inst, Callee, CSCond);

    if (InlinedVals.empty()) {
      auto ReceiverVal = GetProperty<PropertySymExpr>(Var(OutNode));
      Regs[OutNode].addValue(ReceiverVal);
    } else {
      Regs[OutNode].setValues(InlinedVals);
    }
  }

  taintProcessCall(Inst, Callee, Smry.getTaintSmry());
  queryProcessCall(Inst, Callee, Smry.getQueryToTraces());
}

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

void AnalysisState::addFormalToRealLenMap(GuardedValueFlowCallSite *GraphCS,
                                          Function *Callee,
                                          const AnalysisSummary &Smry) {
  (void)Callee;
  auto *CS = GraphCS->getInstruction();
  for (const auto &P : Smry.getStrState().getLenPts()) {
    auto LenV = P.first;
    ProgramValuePtr LenVAtCaller = CStringState::getLenVAtCaller(LenV, CS);
    FormalToRealLenMap[CS].insert(std::make_pair(LenV, LenVAtCaller));
    FormalToRealMap[CS].insert(std::make_pair(LenV, LenVAtCaller));
  }
}

void AnalysisState::addFormalToRealEscapeMap(GuardedValueFlowCallSite *GraphCS,
                                             Function *Callee,
                                             const AnalysisSummary &Smry) {
  (void)Callee;
  const auto &EscapeAllocToSizes = Smry.getEscapeInfo();
  auto *CS = GraphCS->getInstruction();

  // Ensure the entry exists even if we can't (or don't) populate it.
  FormalToRealEscapeMap[CS];

  for (const auto &Iter : EscapeAllocToSizes) {
    (void)Iter;
    // Escape-object mapping previously depended on builder helper APIs
    // that are no longer present. Conservatively skip inlining escapes.
    // Precision loss is acceptable; callers must tolerate empty escape maps.
  }
}

bool AnalysisState::hasFormal(Instruction *CS,
                              const ProgramValuePtr &Formal) const {
  return FormalToRealMap.count(CS) && FormalToRealMap.at(CS).count(Formal);
}

ProgramValuePtr
AnalysisState::getRealForFormal(Instruction *CS,
                                const ProgramValuePtr &Formal) const {
  return FormalToRealMap.at(CS).at(Formal);
}

void AnalysisState::addFormalToRealArgMap(GuardedValueFlowCallSite *GraphCS,
                                          Function *Callee,
                                          const AnalysisSummary &Smry) {
  auto *CS = GraphCS->getInstruction();
  auto *CalleeGraph = Smry.getGraph();
  for (auto Iter = GraphCS->input_begin(Callee),
            EIter = GraphCS->input_end(Callee);
       Iter != EIter; ++Iter) {
    auto *InputNode = (*Iter).InputNode;
    bool IsCommon = (*Iter).IsCommonInput;
    // build the map arg_node => input_node
    if (IsCommon) {
      auto Index = (*Iter).InputIndex;
      if (Index >= CalleeGraph->getNumCommonArgument()) {
        for (size_t I = 0; I < CalleeGraph->getNumVarArgument(); ++I) {
          ProgramValuePtr Formal(CalleeGraph->getVarArgument(I));
          FormalToRealArgMap[CS].insert(std::make_pair(Formal, InputNode));
          FormalToRealMap[CS].insert(std::make_pair(Formal, InputNode));
        }
      } else {
        ProgramValuePtr Formal(CalleeGraph->getCommonArgument(Index));
        FormalToRealArgMap[CS].insert(std::make_pair(Formal, InputNode));
        FormalToRealMap[CS].insert(std::make_pair(Formal, InputNode));
      }
    } else {
      auto Index = (*Iter).InputIndex;
      assert(Index < CalleeGraph->getNumPseudoArgument());
      ProgramValuePtr Formal(CalleeGraph->getPseudoArgument(Index));
      FormalToRealArgMap[CS].insert(std::make_pair(Formal, InputNode));
      FormalToRealMap[CS].insert(std::make_pair(Formal, InputNode));
    }
  }
}

void AnalysisState::addFormalToRealValues(GuardedValueFlowCallSite *GraphCS,
                                          Function *Callee,
                                          const ProgramValuePtr &Formal,
                                          const ProgramValuePtr &Real) {
  auto *CS = GraphCS->getInstruction();
  auto &FormalToRealSymM = FormalToRealSymValsMap[CS];
  auto &SMTMap = SMTRenameCtxMap[Callee];

  GuardedSymbolicValSet RealSyms;
  if (Real.isConstant()) {
    RealSyms.addValue(GetProperty<PropertyInteger>(Real.getAsConstant()));
  } else {
    assert(Regs.count(Real));
    RealSyms.addValues(Regs.at(Real));
  }
  FormalToRealSymM[Formal].addValues(RealSyms);

  // The rename context ties a callee formal to the caller-side SMT expression
  // that should replace it. Summary import reuses this context for conditions,
  // symbolic values, and deferred queries so every translated fact refers to
  // the same call-site specific variables.
  if (!SMTMap.count(Formal)) {
    auto LE = Solver->getExpr(Var(Formal));
    assert(LE.isBitVector());
    assert(LE.getBitVecSize());
    SMTMap.insert(std::make_pair(Formal, RenameCtx(LE)));
  }

  auto &Ctx = SMTMap.at(Formal);
  unsigned LSz = Ctx.getFormalExpr().getBitVecSize();
  SMTExpr RE = Solver->getExpr(Var(Real));
  assert(RE.isBitVector());

  unsigned RSz = RE.getBitVecSize();

  // do some fixes here because currenlty we donot model bitwidth in our
  // domain.
  if (LSz == RSz) {
    Ctx.add(CS, RE);
  } else if (LSz > RSz) {
    auto Temp = RE.basic_sext(LSz - RSz);
    assert(Temp.getBitVecSize() == LSz);
    Ctx.add(CS, Temp);
  } else { // LSz < RSz
    auto Temp = RE.basic_extract(LSz - 1, 0);
    assert(Temp.getBitVecSize() == LSz);
    Ctx.add(CS, Temp);
  }
}

void AnalysisState::initializeFormalToRealMap(CallInst *Inst,
                                              const AnalysisSummary &Smry) {
  // make sure key is in the map
  FormalToRealMap[Inst];
  FormalToRealSymValsMap[Inst];

  FormalToRealArgMap[Inst];
  FormalToRealLenMap[Inst];
  FormalToRealEscapeMap[Inst];

  SMTRenameCtxMap[Smry.getFunc()];
}

Condition AnalysisState::transCond(Instruction *CS, Function *Callee,
                                   const Condition &CalleeCond) const {
  if (CalleeCond.isLit()) {
    return CalleeCond;
  }

  int64_t CondIndex = CalleeCond.getIndex();
  if (InlineCondCache.count(CS) && InlineCondCache.at(CS).count(CondIndex)) {
    return InlineCondCache.at(CS).at(CondIndex);
  }

  // Conditions are imported by translating them onto the caller solver, then
  // renaming any callee-local symbols with the call-site suffix and the formal-
  // to-real mapping constraints. The cache is keyed by call site and callee
  // condition identity because the same summary fact may be used repeatedly
  // while inlining points-to, scalar, taint, and query information.
  bool ToUnknown = false;
  Condition CurCons(CalleeCond.translate(getSolver(), ToUnknown));
  if (!ToUnknown) {
    std::string CSSuffix = getCallsiteSuffix(CS);
    CurCons = CurCons.rename(CSSuffix);
    Condition MapCond = getMappingCond(CS, Callee);
    CurCons = CurCons && MapCond;
  }

  InlineCondCache[CS][CondIndex] = CurCons;
  return CurCons;
}

GuardedSymbolicValSet
AnalysisState::inlineVals(const GuardedSymbolicValSet &Vals, Instruction *CS,
                          Function *Callee, const Condition &CSCond) const {
  // This is the common scalar-summary import path. Each summarized value is
  // re-expressed in caller variables, then guarded by both the translated
  // callee condition and the local call-site condition before it is merged into
  // caller state.
  return Vals.reduce<GuardedSymbolicValSet>(
      [&](const PropertyValuePtr &Val, const Condition &C,
          GuardedSymbolicValSet &Res) {
        auto InlinedV = inlineExpr(Val, CS);
        Res.addValues(InlinedV && transCond(CS, Callee, C) && CSCond);
      },
      [](const GuardedSymbolicValSet &Res) { return Res.isFull(); });
}

QuerySet AnalysisState::inlineVals(const QuerySet &Vals, Instruction *CS,
                                   Function *Callee) const {
  // Deferred bug queries are imported the same way as other summary facts, but
  // each query may expand into multiple caller-side queries after formal
  // substitution. The cache avoids rebuilding those bug predicates every time a
  // summary is reused at the same call site.
  auto Res = Vals.reduce<QuerySet>(
      [&](const NumericalQueryPtr &Q, const Condition &CalleeCond,
          QuerySet &Res) {
        auto *QPtr = Q.get();

        QuerySet InlinedQs;
        if (InlineQueryCache.count(QPtr) &&
            InlineQueryCache.at(QPtr).count(CS)) {
          InlinedQs = InlineQueryCache.at(QPtr).at(CS);
        } else {
          InlinedQs = inlineQuery(Q, CS, Callee);
          InlineQueryCache[QPtr][CS] = InlinedQs;
        }

        InlinedQs = InlinedQs && transCond(CS, Callee, CalleeCond);
        Res.addValues(InlinedQs);
      },
      [](const QuerySet &Res) { return Res.isFull(); });
  return Res;
}

std::pair<PtsSet, bool>
AnalysisState::inlineVals(const PtsSet &Vals, Instruction *CS, Function *Callee,
                          const Condition &CSCond) const {
  const auto &FormalToReal = FormalToRealMap.at(CS);
  bool Degenerate = false;

  // Points-to import is where summary facts become heap facts again. Formal
  // base objects are rebound to the caller's points-to set, offsets are
  // translated into caller expressions, and the result is guarded by both the
  // translated callee condition and the caller's local path condition.
  auto PtsRes = Vals.reduce<PtsSet>(
      [&](const PTItem &Pt, const Condition &CalleeCond, PtsSet &Res) {
        if (Res.isFull()) {
          return;
        }

        auto Base = Pt.getAllocSite();
        auto BaseOff = Pt.getOffset();
        if (FormalToReal.count(Base)) {
          const auto &CallerBase = FormalToReal.at(Base);
          if (!hasPts(CallerBase)) {
            if (mustBeConstantInt(CallerBase)) {
              Degenerate = true;
            }
            return;
          }

          auto PreCond = transCond(CS, Callee, CalleeCond) && CSCond;
          auto CallerOffsets = inlineExpr(BaseOff, CS);

          auto CallerPts =
              getPts(CallerBase).offsetBy(CallerOffsets) && PreCond;
          Res.addValues(CallerPts);
        }
      },
      [](const PtsSet &Res) { return Res.isFull(); });

  Degenerate = Degenerate && PtsRes.empty();
  return std::make_pair(PtsRes, Degenerate);
}

GuardedProgramValSet AnalysisState::inlineVals(const GuardedProgramValSet &Vals,
                                               Instruction *CS,
                                               Function *Callee) const {
  const auto &FormalToReal = FormalToRealMap.at(CS);
  return Vals.reduce<GuardedProgramValSet>(
      [&](const ProgramValuePtr &V, const Condition &CalleeCond,
          GuardedProgramValSet &Res) {
        if (FormalToReal.count(V)) {
          Res.addValue(FormalToReal.at(V), transCond(CS, Callee, CalleeCond));
        }
      },
      [](const GuardedProgramValSet &Res) { return Res.isFull(); });
}
