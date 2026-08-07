#ifndef NPA_SOLVE_CONTEXT_H
#define NPA_SOLVE_CONTEXT_H

/**
 * \file
 * \brief Per-solve bookkeeping and worker execution-context propagation.
 */

#include "Dataflow/NPA/Core/DomainExecution.h"
#include "Dataflow/NPA/Solver/Options.h"
#include "Dataflow/NPA/Solver/Statistics.h"

#include <atomic>
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
    hit_outer_limit_.store(false, std::memory_order_relaxed);
    hit_linear_limit_.store(false, std::memory_order_relaxed);
    hit_fixpoint_limit_.store(false, std::memory_order_relaxed);
  }

  void note_outer_limit_hit() {
    hit_outer_limit_.store(true, std::memory_order_relaxed);
  }

  void note_linear_limit_hit() {
    hit_linear_limit_.store(true, std::memory_order_relaxed);
  }

  void note_fixpoint_limit_hit() {
    hit_fixpoint_limit_.store(true, std::memory_order_relaxed);
  }

  ApproximationSourceFlags snapshot() const {
    ApproximationSourceFlags flags;
    flags.hit_outer_limit = hit_outer_limit_.load(std::memory_order_relaxed);
    flags.hit_linear_limit = hit_linear_limit_.load(std::memory_order_relaxed);
    flags.hit_fixpoint_limit =
        hit_fixpoint_limit_.load(std::memory_order_relaxed);
    return flags;
  }

private:
  std::atomic<bool> hit_outer_limit_{false};
  std::atomic<bool> hit_linear_limit_{false};
  std::atomic<bool> hit_fixpoint_limit_{false};
};

class AdaptiveSccSolveCollector {
public:
  void reset() {
    used_.store(false, std::memory_order_relaxed);
    direct_count_.store(0, std::memory_order_relaxed);
    worklist_count_.store(0, std::memory_order_relaxed);
    tensor_count_.store(0, std::memory_order_relaxed);
    tensor_fallback_count_.store(0, std::memory_order_relaxed);
  }

  void note_used() { used_.store(true, std::memory_order_relaxed); }

  void note_direct(int count = 1) {
    if (count > 0)
      direct_count_.fetch_add(count, std::memory_order_relaxed);
  }

  void note_worklist(int count = 1) {
    if (count > 0)
      worklist_count_.fetch_add(count, std::memory_order_relaxed);
  }

  void note_tensor(int count = 1) {
    if (count > 0)
      tensor_count_.fetch_add(count, std::memory_order_relaxed);
  }

  void note_tensor_fallback(int count = 1) {
    if (count > 0)
      tensor_fallback_count_.fetch_add(count, std::memory_order_relaxed);
  }

  AdaptiveSccSolveStats snapshot() const {
    AdaptiveSccSolveStats stats;
    stats.used = used_.load(std::memory_order_relaxed);
    stats.direct_count = direct_count_.load(std::memory_order_relaxed);
    stats.worklist_count = worklist_count_.load(std::memory_order_relaxed);
    stats.tensor_count = tensor_count_.load(std::memory_order_relaxed);
    stats.tensor_fallback_count =
        tensor_fallback_count_.load(std::memory_order_relaxed);
    return stats;
  }

private:
  std::atomic<bool> used_{false};
  std::atomic<int> direct_count_{0};
  std::atomic<int> worklist_count_{0};
  std::atomic<int> tensor_count_{0};
  std::atomic<int> tensor_fallback_count_{0};
};

inline ApproximationSourceCollector &npa_default_approximation_collector() {
  static thread_local ApproximationSourceCollector collector;
  return collector;
}

inline AdaptiveSccSolveCollector &npa_default_adaptive_scc_collector() {
  static thread_local AdaptiveSccSolveCollector collector;
  return collector;
}

inline ApproximationSourceCollector *&npa_active_approximation_collector_slot() {
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

/// Owns options, results, domain state, and mutable bookkeeping for one solve.
template <class D> class SolveContext {
public:
  using domain_state_type = typename DomainExecutionStateTraits<D>::state_type;

  explicit SolveContext(SolveOptions solve_options = {})
      : options(std::move(solve_options)),
        domain_state(DomainExecutionStateTraits<D>::capture()),
        approximation_scope_(approximation_collector_),
        adaptive_scope_(adaptive_collector_) {
    approximation_collector_.reset();
    adaptive_collector_.reset();
  }

  SolveOptions options;
  Stat stats;
  domain_state_type domain_state;

private:
  ApproximationSourceCollector approximation_collector_;
  AdaptiveSccSolveCollector adaptive_collector_;
  ScopedApproximationSourceCollector approximation_scope_;
  ScopedAdaptiveSccSolveCollector adaptive_scope_;
};

template <class D> struct ExecutionContext {
  using domain_state_type = typename DomainExecutionStateTraits<D>::state_type;

  ApproximationSourceCollector *approximation_collector = nullptr;
  domain_state_type domain_state{};
};

template <class D> inline ExecutionContext<D> capture_execution_context() {
  ExecutionContext<D> ctx;
  ctx.approximation_collector = &npa_active_approximation_collector();
  ctx.domain_state = DomainExecutionStateTraits<D>::capture();
  return ctx;
}

template <class D> class ScopedExecutionContext {
public:
  using traits_type = DomainExecutionStateTraits<D>;

  explicit ScopedExecutionContext(const ExecutionContext<D> &ctx)
      : approx_scope_(*ctx.approximation_collector), domain_scope_(ctx.domain_state) {}

private:
  ScopedApproximationSourceCollector approx_scope_;
  typename traits_type::scope_type domain_scope_;
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

#endif // NPA_SOLVE_CONTEXT_H
