/*
 * IFDS/IDE Solver Configuration
 *
 * Configuration options for the IFDS/IDE solving process (aligned with Phasar's
 * IFDSIDESolverConfig where applicable).
 */

#pragma once

#include <cstdint>

namespace ifds {

enum class SolverConfigOptions : uint32_t {
  None = 0,
  FollowReturnsPastSeeds = 1,
  RecordEdges = 2,
  All = ~0U
};

/// Configuration for IFDS/IDE solver behavior.
struct IFDSIDESolverConfig {
  IFDSIDESolverConfig() = default;
  explicit IFDSIDESolverConfig(SolverConfigOptions options) noexcept
      : m_options(static_cast<uint32_t>(options)) {}

  /// When true, propagate return flow to callers' return sites even when the
  /// callee (start_fact) had no incoming call edge (e.g. entry-point function
  /// returning). Default: false.
  bool follow_returns_past_seeds() const {
    return (m_options & static_cast<uint32_t>(SolverConfigOptions::FollowReturnsPastSeeds)) != 0;
  }
  void set_follow_returns_past_seeds(bool set = true) {
    if (set)
      m_options |= static_cast<uint32_t>(SolverConfigOptions::FollowReturnsPastSeeds);
    else
      m_options &= ~static_cast<uint32_t>(SolverConfigOptions::FollowReturnsPastSeeds);
  }

  /// When true, record computed path edges for debugging/export. May increase
  /// memory. Default: false.
  bool record_edges() const {
    return (m_options & static_cast<uint32_t>(SolverConfigOptions::RecordEdges)) != 0;
  }
  void set_record_edges(bool set = true) {
    if (set)
      m_options |= static_cast<uint32_t>(SolverConfigOptions::RecordEdges);
    else
      m_options &= ~static_cast<uint32_t>(SolverConfigOptions::RecordEdges);
  }

  void set_config(SolverConfigOptions opt) { m_options = static_cast<uint32_t>(opt); }

private:
  uint32_t m_options = 0;
};

} // namespace ifds
