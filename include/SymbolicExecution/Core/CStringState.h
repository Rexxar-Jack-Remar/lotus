/** @file CStringState.h @brief C-string length tracking state for symbolic execution. */
#pragma once

#include "SymbolicExecution/Core/GuardedValue.h"

#include <unordered_map>

namespace SymbolicExecution {

class AnalysisState;
class AnalysisSummary;

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

} // namespace SymbolicExecution

