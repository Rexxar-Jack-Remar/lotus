//===----------------------------------------------------------------------===//
//
// C-string length tracking state implementations.
//
//===----------------------------------------------------------------------===//

#include "SymbolicExecution/Core/CStringState.h"

#include "SymbolicExecution/Core/AnalysisState.h"
#include "SymbolicExecution/Core/PropertyAllocator.h"
#include "SymbolicExecution/Core/PropertyInteger.h"
#include "SymbolicExecution/Integration/GVFGUtility.h"

using namespace llvm;
using namespace SymbolicExecution;

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
