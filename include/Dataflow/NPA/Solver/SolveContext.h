#pragma once

/**
 * \file
 * \brief Per-solve bookkeeping for convergence and adaptive SCC statistics.
 */

#include "Dataflow/NPA/Solver/Options.h"
#include "Dataflow/NPA/Solver/Statistics.h"

#include <utility>

namespace npa {

struct ApproximationSourceFlags {
  bool hit_outer_limit = false;
  bool hit_linear_limit = false;
  bool hit_fixpoint_limit = false;
};

struct AdaptiveSccSolveStats {
  bool used = false;
  int direct_count = 0;
  int worklist_count = 0;
  int tensor_count = 0;
  int tensor_fallback_count = 0;
};

class ApproximationSourceCollector {
public:
  void reset() {
    hit_outer_limit_ = false;
    hit_linear_limit_ = false;
    hit_fixpoint_limit_ = false;
  }

  void note_outer_limit_hit() { hit_outer_limit_ = true; }

  void note_linear_limit_hit() { hit_linear_limit_ = true; }

  void note_fixpoint_limit_hit() { hit_fixpoint_limit_ = true; }

  ApproximationSourceFlags snapshot() const {
    return {hit_outer_limit_, hit_linear_limit_, hit_fixpoint_limit_};
  }

private:
  bool hit_outer_limit_ = false;
  bool hit_linear_limit_ = false;
  bool hit_fixpoint_limit_ = false;
};

class AdaptiveSccSolveCollector {
public:
  void reset() {
    used_ = false;
    direct_count_ = 0;
    worklist_count_ = 0;
    tensor_count_ = 0;
    tensor_fallback_count_ = 0;
  }

  void note_used() { used_ = true; }

  void note_direct(int count = 1) {
    if (count > 0)
      direct_count_ += count;
  }

  void note_worklist(int count = 1) {
    if (count > 0)
      worklist_count_ += count;
  }

  void note_tensor(int count = 1) {
    if (count > 0)
      tensor_count_ += count;
  }

  void note_tensor_fallback(int count = 1) {
    if (count > 0)
      tensor_fallback_count_ += count;
  }

  AdaptiveSccSolveStats snapshot() const {
    return {used_, direct_count_, worklist_count_, tensor_count_,
            tensor_fallback_count_};
  }

private:
  bool used_ = false;
  int direct_count_ = 0;
  int worklist_count_ = 0;
  int tensor_count_ = 0;
  int tensor_fallback_count_ = 0;
};

inline ApproximationSourceCollector &npa_default_approximation_collector() {
  static thread_local ApproximationSourceCollector collector;
  return collector;
}

inline AdaptiveSccSolveCollector &npa_default_adaptive_scc_collector() {
  static thread_local AdaptiveSccSolveCollector collector;
  return collector;
}

inline ApproximationSourceCollector *&
npa_active_approximation_collector_slot() {
  static thread_local ApproximationSourceCollector *collector = nullptr;
  return collector;
}

inline AdaptiveSccSolveCollector *&npa_active_adaptive_scc_collector_slot() {
  static thread_local AdaptiveSccSolveCollector *collector = nullptr;
  return collector;
}

inline ApproximationSourceCollector &npa_active_approximation_collector() {
  ApproximationSourceCollector *collector =
      npa_active_approximation_collector_slot();
  return collector ? *collector : npa_default_approximation_collector();
}

inline AdaptiveSccSolveCollector &npa_active_adaptive_scc_collector() {
  AdaptiveSccSolveCollector *collector =
      npa_active_adaptive_scc_collector_slot();
  return collector ? *collector : npa_default_adaptive_scc_collector();
}

class ScopedApproximationSourceCollector {
public:
  explicit ScopedApproximationSourceCollector(
      ApproximationSourceCollector &collector)
      : previous_(npa_active_approximation_collector_slot()) {
    npa_active_approximation_collector_slot() = &collector;
  }

  ~ScopedApproximationSourceCollector() {
    npa_active_approximation_collector_slot() = previous_;
  }

private:
  ApproximationSourceCollector *previous_;
};

class ScopedAdaptiveSccSolveCollector {
public:
  explicit ScopedAdaptiveSccSolveCollector(AdaptiveSccSolveCollector &collector)
      : previous_(npa_active_adaptive_scc_collector_slot()) {
    npa_active_adaptive_scc_collector_slot() = &collector;
  }

  ~ScopedAdaptiveSccSolveCollector() {
    npa_active_adaptive_scc_collector_slot() = previous_;
  }

private:
  AdaptiveSccSolveCollector *previous_;
};

/// Owns options, results, and mutable bookkeeping for one solve.
template <class D> class SolveContext {
public:
  explicit SolveContext(SolveOptions solve_options = {})
      : options(std::move(solve_options)),
        approximation_scope_(approximation_collector_),
        adaptive_scope_(adaptive_collector_),
        convergence_scope_(options.convergence_policy) {
    approximation_collector_.reset();
    adaptive_collector_.reset();
  }

  SolveOptions options;
  Stat stats;

private:
  ApproximationSourceCollector approximation_collector_;
  AdaptiveSccSolveCollector adaptive_collector_;
  ScopedApproximationSourceCollector approximation_scope_;
  ScopedAdaptiveSccSolveCollector adaptive_scope_;
  ScopedConvergencePolicy convergence_scope_;
};

inline ApproximationSourceFlags npa_approximation_source_flags() {
  return npa_active_approximation_collector().snapshot();
}

inline void npa_reset_limit_hit() {
  npa_active_approximation_collector().reset();
}

inline void npa_note_outer_limit_hit() {
  npa_active_approximation_collector().note_outer_limit_hit();
}

inline void npa_note_linear_limit_hit() {
  npa_active_approximation_collector().note_linear_limit_hit();
}

inline void npa_note_fixpoint_limit_hit() {
  npa_active_approximation_collector().note_fixpoint_limit_hit();
}

inline bool npa_hit_outer_limit() {
  return npa_approximation_source_flags().hit_outer_limit;
}

inline bool npa_hit_linear_limit() {
  return npa_approximation_source_flags().hit_linear_limit;
}

inline bool npa_hit_fixpoint_limit() {
  return npa_approximation_source_flags().hit_fixpoint_limit;
}

inline bool npa_limit_hit() {
  const auto flags = npa_approximation_source_flags();
  return flags.hit_outer_limit || flags.hit_linear_limit ||
         flags.hit_fixpoint_limit;
}

inline AdaptiveSccSolveStats npa_adaptive_scc_solve_stats() {
  return npa_active_adaptive_scc_collector().snapshot();
}

inline void npa_reset_adaptive_scc_stats() {
  npa_active_adaptive_scc_collector().reset();
}

inline void npa_note_adaptive_scc_used() {
  npa_active_adaptive_scc_collector().note_used();
}

inline void npa_note_adaptive_scc_direct(int count = 1) {
  npa_active_adaptive_scc_collector().note_direct(count);
}

inline void npa_note_adaptive_scc_worklist(int count = 1) {
  npa_active_adaptive_scc_collector().note_worklist(count);
}

inline void npa_note_adaptive_scc_tensor(int count = 1) {
  npa_active_adaptive_scc_collector().note_tensor(count);
}

inline void npa_note_adaptive_scc_tensor_fallback(int count = 1) {
  npa_active_adaptive_scc_collector().note_tensor_fallback(count);
}

} // namespace npa

