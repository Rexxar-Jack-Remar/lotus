#pragma once

#include "Alias/InclusionBased/TPA/PointerAnalysis/Analysis/PointerAnalysis.h"
#include "Alias/InclusionBased/TPA/PointerAnalysis/Support/Env.h"
#include "Alias/InclusionBased/TPA/PointerAnalysis/Support/Memo.h"

namespace tpa {

class SemiSparseProgram;

// Semi-sparse flow- and context-sensitive pointer analysis implementation
//
// This is the main pointer analysis algorithm in TPA. It performs:
// - Inclusion-based (Andersen-style) pointer analysis
// - Flow-sensitive analysis (respects program order)
// - Context-sensitive analysis (distinguishes call contexts)
// - Semi-sparse representation (only analyzes relevant program points)
//
// The analysis uses:
// - Env: Points-to sets for top-level pointers (variables, function parameters)
// - Memo: Memoization of analysis results to avoid redundant computation
// - Worklist: Iterative propagation until fixpoint
//
// Analysis Flow:
//   1. Build semi-sparse program from LLVM IR
//   2. Initialize global variables and special pointers
//   3. Run worklist-based data flow analysis
//   4. Return points-to sets for queries
class SemiSparsePointerAnalysis
    : public PointerAnalysis<SemiSparsePointerAnalysis> {
private:
  // Environment: maps top-level pointers to their points-to sets
  // Key: Pointer (context, value), Value: PtsSet (set of memory objects)
  Env env;
  // Memoization table for analysis results
  // Prevents redundant recomputation of the same analysis state
  Memo memo;

public:
  SemiSparsePointerAnalysis() = default;

  // Run the pointer analysis on a program
  // Parameters: ssProg - the semi-sparse program representation
  // Side effects: populates env and memo with analysis results
  void runOnProgram(const SemiSparseProgram &);

  // Implementation of getPtsSet for CRTP pattern
  // Returns the points-to set for a given pointer from the env
  PtsSet getPtsSetImpl(const Pointer *) const;
};

} // namespace tpa
