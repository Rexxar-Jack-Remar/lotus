/** @file Trace.h @brief Bug trace steps collected during symbolic execution. */
#ifndef ANALYSIS_SYMBOLICEXECUTION_CORE_TRACE_H
#define ANALYSIS_SYMBOLICEXECUTION_CORE_TRACE_H

#include <llvm/IR/Instruction.h>
#include <llvm/IR/Value.h>

namespace SymbolicExecution {

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

} // namespace SymbolicExecution

#endif
