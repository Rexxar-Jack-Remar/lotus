/** @file TaintState.h @brief Taint facts and trace steps tracked by symbolic execution. */
#ifndef ANALYSIS_SYMBOLICEXECUTION_CORE_TAINTSTATE_H
#define ANALYSIS_SYMBOLICEXECUTION_CORE_TAINTSTATE_H

#include "SymbolicExecution/Core/GuardedValue.h"

#include <unordered_map>
#include <vector>

namespace SymbolicExecution {

class TaintStep;

class TaintValSet : public GuardedSet<TaintValSet, ProgramValuePtr,
                                      &AnalysisLimit::TAINT_VAL_SET_LIMIT_V> {};

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

/// Summary of taint facts exported from one analyzed function.
///
/// The summary records which formals and returns are tainted, plus the steps
/// needed to reconstruct a taint trace after the summary is reused at a call
/// site.
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

} // namespace SymbolicExecution

#endif
