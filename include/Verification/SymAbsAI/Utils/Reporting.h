/**
 * @file Reporting.h
 * @brief Helpers for printing SymAbsAI configuration and analysis results.
 *
 * Provides free functions for uniform pretty-printing of effective
 * configuration settings, entry-point abstract states, per-block results,
 * and exit-block results.
 */
#pragma once

#include <string>

namespace llvm {
class Function;
} // namespace llvm

namespace symabs_ai {
class Analyzer;
} // namespace symabs_ai

/** @brief Print a concise, uniform "Effective configuration" section. */
void printEffectiveConfiguration(
    const std::string &configSource, const std::string &domainName,
    const std::string &domainSource, bool fallbackToFirst,
    const std::string &fragmentStrategy, const std::string &fragmentOrigin,
    const std::string &analyzerVariant, bool incremental, int wideningDelay,
    int wideningFrequency, const std::string &wideningOrigin,
    const std::string &memoryVariant, int addressBits,
    const std::string &memoryOrigin);

/** @brief Pretty-print analysis results at the function entry. */
void printEntryResult(symabs_ai::Analyzer *analyzer, llvm::Function *func);

/** @brief Pretty-print analysis results for all basic blocks. */
void printAllBlocksResults(symabs_ai::Analyzer *analyzer, llvm::Function *func);

/** @brief Pretty-print analysis results for exit blocks (with return). */
void printExitBlocksResults(symabs_ai::Analyzer *analyzer,
                            llvm::Function *func);
