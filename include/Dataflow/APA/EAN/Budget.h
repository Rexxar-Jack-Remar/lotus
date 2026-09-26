#pragma once

// Budget and stop bookkeeping for the saturation driver (paper §III.C:
// B = ⟨B_N, B_M, B_R, B_T, B_P⟩ bounding e-nodes, applied matches, rounds,
// wall-clock time, and consecutive rounds without an extraction improvement).
//
// The driver is anytime: any stopping point yields a valid extractable result.

#include <cstddef>
#include <limits>

namespace elimination {
namespace ean {

struct Budget {
  std::size_t nodeLimit = std::numeric_limits<std::size_t>::max();  // B_N (g.totalSize())
  std::size_t matchLimit = std::numeric_limits<std::size_t>::max(); // B_M (declared; per-match enforcement lands in M5)
  std::size_t roundLimit = std::numeric_limits<std::size_t>::max(); // B_R
  std::size_t plateauLimit = std::numeric_limits<std::size_t>::max(); // B_P
  double timeLimitSec = 1e30;                                       // B_T

  // A generous default: effectively unbounded, so saturation runs to fixpoint.
  static Budget unbounded() { return Budget{}; }
};

enum class StopReason {
  Saturated,   // no phase produced a change
  RoundLimit,  // B_R hit
  NodeLimit,   // B_N hit
  Plateau,     // B_P consecutive rounds without cost improvement
  TimeLimit,   // B_T hit
};

struct SaturationStats {
  std::size_t rounds = 0;
  std::size_t peakNodes = 0;
  StopReason stop = StopReason::Saturated;
  double initCost = 0.0;
  double finalCost = 0.0;
};

} // namespace ean
} // namespace elimination

