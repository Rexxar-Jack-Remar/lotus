//===----------------------------------------------------------------------===//
//
// AnalysisState utility implementations.
// This file holds the small abstractions that keep the core transfer logic from
// being buried in container bookkeeping. Most helpers here preserve guarded
// symbolic facts while moving between points-to items, value sets, and the
// conservative CStringState cache.
//
//===----------------------------------------------------------------------===//

#include "llvm/IR/CFG.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"

#include "SymbolicExecution/AnalysisDriver.h"
#include "SymbolicExecution/AnalysisState.h"
#include "SymbolicExecution/DomTreePass.h"
#include "SymbolicExecution/InstResolver.h"
#include "SymbolicExecution/MemoryAPI.h"
#include "SymbolicExecution/PropertyAllocator.h"
#include "SymbolicExecution/PropertyInteger.h"
#include "SymbolicExecution/PropertySym.h"
#include "SymbolicExecution/TaintModel.h"

#include <functional>
#include <numeric>

#define DEBUG_TYPE "Symex"

using namespace SymbolicExecution;

namespace std {
size_t hash<SymbolicExecution::Condition>::operator()(
    const SymbolicExecution::Condition &V) const {
  return V.hash();
}

size_t hash<SymbolicExecution::AccessPath>::operator()(
    const SymbolicExecution::AccessPath &V) const {
  return V.hash();
}

size_t hash<SymbolicExecution::PTItem>::operator()(
    const SymbolicExecution::PTItem &V) const {
  return V.hash();
}
} // namespace std

PTItem::PTItem(ProgramValuePtr AllocSite, MemObjKind K, int64_t Off,
               const PropertyValuePtr &Sz)
    : AP(AllocSite, GetProperty<PropertyInteger>(Off)), Size(Sz), K(K) {}

bool PTItem::isSymbolic() const { return K == MK_SYMBOLIC; }

bool PTItem::isConcrete() const { return K == MK_CONCRETE; }

bool PTItem::isPlaceHolder() const { return K == MK_PLACEHOLDER; }

bool PTItem::isOffsetSymbolic() const {
  return IsaProperty<PropertySymExpr>(getOffset());
}

BigInteger PTItem::getConstOffset() const {
  return CastProperty<PropertyInteger>(getOffset())->getVal();
}

PTItem PTItem::offsetBy(const PropertyValuePtr &Off) const {
  PTItem Res(*this);
  Res.AP.Offset = Res.AP.Offset + Off;
  return Res;
}

PTItem PTItem::offsetBy(int64_t Off) const {
  PTItem Res(*this);
  Res.AP.Offset = Res.AP.Offset + PropertyInteger(Off);
  return Res;
}

void PTItem::changeOffsetBy(const PropertyValuePtr &Off) {
  AP.Offset = AP.Offset + Off;
}

std::string PTItem::getID() const { return AP.getID(); }

PtsSet PtsSet::offsetBy(const GuardedSymbolicValSet &Offs) const {
  PtsSet Res;

  // The guarded cartesian product is central to symbolic memory updates. Each
  // points-to item is paired with each feasible offset, and the resulting guard
  // remembers which path allows that derived access path.
  forEach2(
      Offs,
      [&](const PTItem &Pt, const PropertyValuePtr &Off,
          const Condition &Cond) { Res.addValue(Pt.offsetBy(Off), Cond); },
      [&]() { return Res.isFull(); });

  return Res;
}

GuardedSymbolicValSet::GuardedSymbolicValSet(const PropertyValuePtr &V,
                                             const Condition &Cond) {
  addValue(V, Cond);
}

GuardedSymbolicValSet
GuardedSymbolicValSet::binOp(const GuardedSymbolicValSet &Rhs,
                             PropertyValue::BinOp Op, size_t Threshold) const {
  GuardedSymbolicValSet Result;

  // These utility combinators are the scalar analogue of state transfer. They
  // preserve path guards while bounding explosion through Threshold so callers
  // can reuse one implementation for arithmetic, offsets, and comparisons.
  forEach2(
      Rhs,
      [&](const PropertyValuePtr &Val1, const PropertyValuePtr &Val2,
          const Condition &Cond) {
        PropertyValuePtr ResVal(Val1->binOp(Val2, Op));
        if (ResVal) {
          Result.addValue(ResVal, Cond);
        }
      },
      [=]() { return Result.size() >= Threshold; });

  return Result;
}

GuardedSymbolicValSet GuardedSymbolicValSet::binOp(const PropertyValue &R,
                                                   PropertyValue::BinOp Op,
                                                   size_t Threshold) const {
  GuardedSymbolicValSet Result;

  forEach(
      [&](const PropertyValuePtr &Val, const Condition &Cond) {
        PropertyValuePtr ResVal(Val->binOp(R, Op));
        if (ResVal) {
          Result.addValue(ResVal, Cond);
        }
      },
      [=]() { return Result.size() >= Threshold; });

  return Result;
}

std::vector<Condition>
GuardedSymbolicValSet::cmp(const GuardedSymbolicValSet &Rhs,
                           unsigned Pred) const {
  std::vector<Condition> Res = {Condition::getFalseCond(),
                                Condition::getFalseCond(),
                                Condition::getFalseCond()};

  // cmp groups pairwise comparison outcomes back into guard sets for false,
  // true, and unknown. Branch transfer can then ask which outcomes stay
  // feasible without re-enumerating the cross product itself.
  std::vector<std::pair<unsigned, Condition>> CmpRes =
      zip2<GuardedSymbolicValSet, unsigned>(
          Rhs, [=](const PropertyValuePtr &Val1, const PropertyValuePtr &Val2) {
            unsigned CmpRes = Val1->cmp(Val2.get(), Pred);
            return CmpRes;
          });

  for (const auto &P : CmpRes) {
    assert(P.first <= 2);
    Res[P.first].orCond(P.second);
  }

  return Res;
}

GuardedSymbolicValSet GuardedSymbolicValSet::seqAdd(
    std::vector<std::tuple<GuardedSymbolicValSet::IterType,
                           GuardedSymbolicValSet::IterType, BigInteger,
                           ProgramValuePtr>>
        Seq,
    PathCondSolver *Solver, bool AddEqCond, const std::string &CSSuffix) {
  GuardedSymbolicValSet Res;
  std::vector<std::pair<PropertyValuePtr, Condition>> Selections(Seq.size());

  // seqAdd incrementally picks one value from each guarded sequence and folds
  // the selected affine terms into a single symbolic result. CString and memory
  // helpers use it when they need one shared routine for guarded linear sums.
  bool Advanced = false;
  do {
    Advanced = false;
    for (size_t I = 0; I < Seq.size(); ++I) {
      // Caution: must use reference type
      IterType &CurIter = std::get<0>(Seq[I]);
      IterType &EndIter = std::get<1>(Seq[I]);
      const BigInteger &Coeff = std::get<2>(Seq[I]);
      ProgramValuePtr Variable = std::get<3>(Seq[I]);

      if (CurIter == EndIter) {
        continue;
      }

      auto Val = CurIter->first * PropertyInteger(Coeff);
      Condition Cond = CurIter->second;
      if (AddEqCond) {
        Condition EqCond(Solver->buildEqualCond(Var(Variable),
                                                CurIter->first.get(), CSSuffix),
                         Solver);
        Cond = Cond && EqCond;
      }
      Selections[I] = std::make_pair(Val, CurIter->second && Cond);
      ++CurIter;
      Advanced = true;
    }

    if (Advanced) {
      auto CurrtentRun = std::accumulate(
          std::next(Selections.begin()), Selections.end(), Selections[0],
          [](const std::pair<PropertyValuePtr, Condition> &L,
             const std::pair<PropertyValuePtr, Condition> &R) {
            auto Val = L.first + R.first;
            assert(Val);
            return std::make_pair(Val, L.second && R.second);
          });
      Res.addValue(CurrtentRun.first, CurrtentRun.second);
    }

  } while (Advanced && !Res.isFull());

  return Res;
}

ProgramValuePtr CStringState::getLenVAtCaller(const ProgramValuePtr &LenV,
                                              Instruction *CS) {
  std::string Name = LenV.getID() + "_" + gvfg_utility::ptrToString(CS);
  ProgramValuePtr LenVAtCaller(LenV.getType(), Name);
  return LenVAtCaller;
}

void CStringState::handleCStrLen(const ProgramValuePtr &Ptr, Instruction *Loc,
                                 const Condition &Cond, AnalysisState &CurState,
                                 bool IsDirect) {
  // CStringState does not attempt a full string domain. Instead it tracks just
  // enough symbolic length information for library summaries and bug queries,
  // falling back to fresh unknowns when the underlying memory reasoning is too
  // imprecise.
  ProgramValuePtr Len = getLenVariable(Ptr, Loc, CurState, IsDirect);
  GuardedSymbolicValSet LenValues;
  if (CurState.hasPts(Ptr)) {
    LenValues =
        computeCStrLength(Len, Loc, CurState.getPts(Ptr), CurState, Cond);
  } else if (CurState.mustBeConstantInt(Ptr)) {
    LenValues.addValue(CurState.getAbsurdStrLen());
  }

  if (LenValues.empty()) {
    LenValues.addValue(Len);
  }

  CurState.addSymbolicVals(Len, LenValues);
  CurState.addUnknownSym(Len);
}

GuardedSymbolicValSet
CStringState::getCStrlen(const ProgramValuePtr &Ptr, Instruction *Loc,
                         bool IsDirect, const AnalysisState &CurState) const {
  ProgramValuePtr Len = getLenVariable(Ptr, Loc, CurState, IsDirect);
  return CurState.getSymbolicVals(Len);
}

ProgramValuePtr CStringState::getLenVariable(const ProgramValuePtr &Ptr,
                                             Instruction *Loc,
                                             const AnalysisState &CurState,
                                             bool IsDirect) const {
  // Direct queries reuse the GVFG node of the length-producing instruction.
  // Indirect ones synthesize a stable auxiliary name so summaries can carry the
  // same symbolic length fact across calls.
  ProgramValuePtr Len;
  if (IsDirect) {
    Len = CurState.getNode(Loc);
  } else {
    std::string Name = Ptr.getID() + "_len_" + gvfg_utility::ptrToString(Loc);
    Len = ProgramValuePtr(AnalysisState::NON_PTR_TY, Name);
  }
  return Len;
}

GuardedSymbolicValSet
CStringState::computeCStrLength(const ProgramValuePtr &Len, Instruction *Loc,
                                const PtsSet &Pts, AnalysisState &CurState,
                                const Condition &Cond) {
  GuardedSymbolicValSet LenValues;
  (void)Cond;

  Pts.forEach(
      [&](const PTItem &Pt, const Condition &Cond) {
        auto CurLenValues = computeCStrLength(Len, Loc, Pt, CurState, Cond);
        LenValues.addValues(CurLenValues);
      },
      [&]() { return LenValues.isFull(); });

  return LenValues;
}

GuardedSymbolicValSet
CStringState::computeCStrLength(const ProgramValuePtr &Len, Instruction *Loc,
                                const PTItem &Pt, AnalysisState &CurState,
                                const Condition &Cond) {
  auto LenValues = CurState.computeCStrLength(Loc, Pt, Cond);
  LenPts[Len].addValue(Pt);
  return LenValues;
}

GuardedSymbolicValSet AnalysisState::computeCStrLength(Instruction *Pos,
                                                       const PTItem &Pt,
                                                       const Condition &Cond) {
  // Older versions of the string-length modeling relied on builder helper
  // APIs (e.g., querying stored-zero offsets, byte loads) that are no longer
  // exposed. Until those helpers are reintroduced, keep this computation
  // conservative.
  (void)Pos;
  (void)Pt;
  (void)Cond;
  return {};
}

void CStringState::onProcessCall(Instruction *Inst, Function *Callee,
                                 AnalysisState &CurState,
                                 const CStringState &Smry) {
  // Summary import re-materializes callee-side length variables in the caller.
  // The inlineVals result tells us which caller points-to items correspond to
  // each summarized string, after which length facts are recomputed locally.
  const Condition &CSCond = CurState.getLocalCond(Inst->getParent());
  for (const auto &P : Smry.getLenPts()) {
    auto LenV = P.first;

    ProgramValuePtr LenVAtCaller = getLenVAtCaller(LenV, Inst);

    GuardedSymbolicValSet CallerLenValues;
    auto Res = CurState.inlineVals(P.second, Inst, Callee, CSCond);
    auto CallerPts = Res.first;
    auto Degenerate = Res.second;

    CallerPts.forEach(
        [&](const PTItem &Pt, const Condition &Cond) {
          auto CurLenValues =
              computeCStrLength(LenVAtCaller, Inst, Pt, CurState, Cond);
          CallerLenValues.addValues(CurLenValues);
        },
        [&]() { return CallerLenValues.isFull(); });

    if (CallerLenValues.empty()) {
      if (Degenerate) {
        CallerLenValues.addValue(CurState.getAbsurdStrLen());
      } else {
        CallerLenValues.addValue(LenVAtCaller);
      }
    }

    CurState.addSymbolicVals(LenVAtCaller, CallerLenValues);
    CurState.addUnknownSym(LenVAtCaller);
  }
}

// Get an absurdly long length value, mark the absence of '\0'
PropertyValuePtr AnalysisState::getAbsurdStrLen() const {
  return GetProperty<PropertyInteger>(BigInteger(1000000));
}

bool SymbolicExecution::isPseudoArgVal(const ProgramValuePtr &V) {
  if (V.isa<GuardedValueFlowNodeValue>()) {
    auto *NV = V.getAs<GuardedValueFlowNodeValue>();
    if (isa<lotus::gvfg::GuardedValueFlowArgumentNode>(NV->getNode())) {
      return true;
    } else {
      return false;
    }
  }

  return false;
}
