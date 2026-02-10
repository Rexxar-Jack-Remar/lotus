/*
 * IFDS/IDE Solver Configuration
 *
 * Configuration options for the IFDS/IDE solving process (aligned with Phasar's
 * IFDSIDESolverConfig where applicable).
 */

#pragma once

#include <cstdint>

#include "Alias/AliasAnalysisWrapper/AliasAnalysisWrapper.h"

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

  /// Automatically construct and inject alias analysis when none was provided
  /// by the client analysis.
  /// Default: false (Phasar-style explicit alias wiring).
  bool auto_inject_alias_analysis() const { return m_auto_inject_alias_analysis; }
  void set_auto_inject_alias_analysis(bool enable = true) {
    m_auto_inject_alias_analysis = enable;
  }

  /// Alias backend used when auto injection is enabled.
  const lotus::AAConfig &alias_analysis_config() const { return m_alias_analysis_config; }
  void set_alias_analysis_config(const lotus::AAConfig &cfg) {
    m_alias_analysis_config = cfg;
  }

  static lotus::AAConfig default_alias_analysis_config() {
    return lotus::AAConfig(lotus::AAConfig::Implementation::BasicAA,
                           lotus::AAConfig::ContextSensitivity::None, 0, true,
                           lotus::AAConfig::Solver::Default);
  }

private:
  uint32_t m_options = 0;
  // Phasar-style default: alias-aware analyses explicitly receive alias info.
  bool m_auto_inject_alias_analysis = false;
  lotus::AAConfig m_alias_analysis_config = default_alias_analysis_config();
};

} // namespace ifds
