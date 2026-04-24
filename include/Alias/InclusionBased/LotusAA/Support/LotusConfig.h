/*
 * LotusAA - Analysis Configuration
 * 
 * Centralized configuration constants and default values
 * for the Lotus alias analysis.
 */

#pragma once

namespace llvm {
namespace LotusConfig {

/**
 * Call Graph Construction Limits
 */
struct CallGraphLimits {
  /// Default maximum iterations for iterative call graph construction
  static constexpr int DEFAULT_MAX_ITERATIONS = 2;
  
  /// Default maximum indirect call targets to consider
  static constexpr int DEFAULT_MAX_TARGETS = 5;
  
  /// Maximum call graph size before applying aggressive pruning
  static constexpr int MAX_CALLGRAPH_SIZE = 10000;
};

/**
 * Inlining and Interprocedural Analysis Limits
 */
struct InterproceduralLimits {
  /// Default maximum inlining depth for context-sensitive analysis
  static constexpr int DEFAULT_INLINE_DEPTH = 2;
  
  /// Maximum function size (instructions) to consider for inlining
  static constexpr int DEFAULT_MAX_INLINE_SIZE = 100;
  
  /// Maximum access path depth in function summaries
  static constexpr int DEFAULT_MAX_ACCESS_PATH_LEVEL = 2;
};

/**
 * Memory Model Configuration
 */
struct MemoryLimits {
  /// Maximum values to track per memory location per basic block
  static constexpr int DEFAULT_MAX_BB_LOAD = 5;
  
  /// Maximum basic block depth for memory tracking
  static constexpr int DEFAULT_MAX_BB_DEPTH = 100;
  
  /// Maximum total values to track per memory location
  static constexpr int DEFAULT_MAX_LOAD = 10;
  
  /// Maximum basic blocks to track for store operations (-1 = unlimited)
  static constexpr int DEFAULT_STORE_DEPTH = -1;
};

/**
 * Analysis Timeouts
 */
struct TimeoutLimits {
  /// Default timeout per function (seconds)
  static constexpr double DEFAULT_PER_FUNCTION_TIMEOUT = 10.0;
};

/**
 * Heuristics Configuration
 */
struct Heuristics {
  /// Enable global pointer initialization heuristic by default
  static constexpr bool DEFAULT_ENABLE_GLOBAL_HEURISTIC = true;
  
  /// Probability thresholds for summary node updates
  /// Index represents access-path depth
  static constexpr int MAX_SUMMARY_AP_DEPTH = 100;
  static constexpr float MEM_CHANGE_PROBABILITY[MAX_SUMMARY_AP_DEPTH + 1] = {
      0, 0.8, 0.4, 0.2, 0.1, 0.05, 0.02, 0.01, 0.005, 0.002, 0.001
  };
  
  /// Probability that a path condition is satisfied
  static constexpr float COND_SAT_PROBABILITY = 0.5f;
};

/**
 * Debug and Output Configuration
 */
struct DebugOptions {
  /// Default: don't print points-to results
  static constexpr bool DEFAULT_PRINT_PTS = false;
  
  /// Default: don't print call graph results
  static constexpr bool DEFAULT_PRINT_CG = false;
  
  /// Default: don't enable call graph construction
  static constexpr bool DEFAULT_ENABLE_CG = false;
};

} // namespace LotusConfig
} // namespace llvm

