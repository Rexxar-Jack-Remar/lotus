#pragma once

#include "Alias/InclusionBased/TPA/PointerAnalysis/Analysis/PointerAnalysis.h"
#include "Alias/InclusionBased/TPA/PointerAnalysis/Support/Env.h"
#include "Alias/InclusionBased/TPA/PointerAnalysis/Support/Memo.h"

#include <string>

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
  struct Config {
    bool parallel = false;
    unsigned threads = 0;   // zero: hardware concurrency; includes the caller
    unsigned lookahead = 4; // predicted transfers per worker
  };
  struct Statistics {
    std::size_t transfers = 0;
    std::size_t evaluations = 0;
    std::size_t retries = 0;
    std::size_t discarded = 0;
    std::size_t batches = 0;
    unsigned threads = 1;
    unsigned peakWorkers = 1;
    double initializationSeconds = 0;
    double solveSeconds = 0;
  };

private:
  Statistics stats;
  const SemiSparseProgram *program = nullptr;

public:
  SemiSparsePointerAnalysis() = default;

  // Run the pointer analysis on a program
  // Parameters: ssProg - the semi-sparse program representation
  // Side effects: populates env and memo with analysis results
  void runOnProgram(const SemiSparseProgram &);
  void runOnProgram(const SemiSparseProgram &, Config);
  const Statistics &getStatistics() const { return stats; }
  // Analyses must use the same SemiSparseProgram and context policy. Object
  // identities are compared by allocation site/context, offset, and summary.
  bool hasSameSolution(const SemiSparsePointerAnalysis &,
                       std::string *difference = nullptr) const;

  // Implementation of getPtsSet for CRTP pattern
  // Returns the points-to set for a given pointer from the env
  PtsSet getPtsSetImpl(const Pointer *) const;
};

} // namespace tpa
