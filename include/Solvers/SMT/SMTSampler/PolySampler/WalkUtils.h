/**
 * @file WalkUtils.h
 * @brief Shared utility functions for polytope walk implementations
 *
 * Centralises helpers that were previously duplicated across every walk .cpp
 * (dot_ld, satisfies_constraints, safe_cast_t).  Including this header in each
 * walk translation unit removes the duplication (fixes L10).
 */

#pragma once

#include <cstdint>
#include <limits>
#include <vector>

#include "Solvers/SMT/SMTSampler/PolySampler/RegionSamplingTypes.h"

namespace RegionSampling {
namespace WalkUtils {

/// Long-double dot product of two int64_t vectors.
inline long double dot_ld(const std::vector<int64_t> &a,
                          const std::vector<int64_t> &b) {
  long double sum = 0.0L;
  for (size_t i = 0; i < a.size(); ++i)
    sum += static_cast<long double>(a[i]) * static_cast<long double>(b[i]);
  return sum;
}

/// Returns true iff every constraint is satisfied by @p point.
inline bool satisfies_constraints(const std::vector<LinearConstraint> &constraints,
                                  const std::vector<int64_t> &point) {
  for (const auto &c : constraints) {
    if (dot_ld(c.coeffs, point) > static_cast<long double>(c.bound))
      return false;
  }
  return true;
}

/**
 * @brief Safely cast a long double to int64_t.
 *
 * Returns false (and leaves @p out unchanged) if the value is not finite or
 * lies outside [INT64_MIN, INT64_MAX].  This fixes the undefined-behaviour
 * casts identified as B25/B26.
 */
inline bool safe_cast_t(long double v, int64_t &out) {
  if (!std::isfinite(v))
    return false;
  // Use the exact representable limits for the comparison.
  constexpr long double kMin =
      static_cast<long double>(std::numeric_limits<int64_t>::min());
  constexpr long double kMax =
      static_cast<long double>(std::numeric_limits<int64_t>::max());
  if (v < kMin || v > kMax)
    return false;
  out = static_cast<int64_t>(v);
  return true;
}

} // namespace WalkUtils
} // namespace RegionSampling
