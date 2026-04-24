/*
 * LotusAA - Configuration Constants
 * 
 * Compile-time configuration parameters for the analysis.
 * These control precision vs. performance trade-offs.
 */

#pragma once

namespace LotusConfig {

// Maximum considered access-path depth in summary nodes
constexpr int MAXIMAL_SUMMARY_AP_DEPTH = 100;

// Probability that a function may change a side-effect value
// Index represents access-path depth
constexpr float MEM_CHANGE_PROBABILITY[MAXIMAL_SUMMARY_AP_DEPTH + 1] = {
    0, 0.8, 0.4, 0.2, 0.1, 0.05, 0.02, 0.01, 0.005, 0.002, 0.001
};

// Probability that a path condition is satisfied
constexpr float COND_SAT_PROBABILITY = 0.5f;

} // namespace LotusConfig


