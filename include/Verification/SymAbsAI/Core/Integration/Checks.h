/**
 * @file Checks.h
 * @brief Checker helpers for assertion violation and memory-safety reporting.
 *
 * Provides free functions that run post-analysis checks on analyzed functions
 * using the SymAbsAI analyzer state, printing results to llvm::outs().
 */
#pragma once

#include <llvm/IR/Function.h>

namespace symabs_ai {
class Analyzer;
} // namespace symabs_ai

/**
 * Runs the assertion violation check on the given function using the
 * provided analyzer state. Prints results to llvm::outs().
 * @param analyzer The SymAbsAI analyzer with computed abstract state.
 * @param targetFunc The function to check for assertion violations.
 * @returns A small non-negative number of violations; clamps large counts.
 */
int runAssertionCheck(symabs_ai::Analyzer *analyzer,
                      llvm::Function *targetFunc);

/**
 * Runs a conservative memory-safety check for load/store pointer validity.
 * Requires RTTI for dynamic_cast on abstract values. Prints results to outs().
 * @param analyzer The SymAbsAI analyzer with computed abstract state.
 * @param targetFunc The function to check for memory-safety violations.
 * @returns A small non-negative number of potential violations; clamps large counts.
 */
int runMemSafetyCheck(symabs_ai::Analyzer *analyzer,
                      llvm::Function *targetFunc);
