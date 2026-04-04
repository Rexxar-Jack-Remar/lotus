#ifndef NPA_RUNTIME_H
#define NPA_RUNTIME_H

/**
 * \file
 * \brief Runtime bookkeeping, execution context, and NPA-specific errors.
 */

#include "Dataflow/NPA/Core/Base/Foundation.h"

#include <atomic>
#include <cassert>
#include <iostream>
#include <stdexcept>

namespace npa {

struct NoopDomainRunState {};

struct NoopDomainRunStateScope {
  explicit NoopDomainRunStateScope(const NoopDomainRunState &) {}
};

template <class D> struct DomainExecutionStateTraits {
  using state_type = NoopDomainRunState;
  using scope_type = NoopDomainRunStateScope;

  static state_type capture() { return {}; }
};

template <class Tag> class DomainWidthContext {
public:
  struct state_type {
    bool active = false;
    unsigned bit_width = 1;
  };

  class scope_type {
  public:
    scope_type() = default;

    explicit scope_type(unsigned bit_width)
        : scope_type(state_type{true, bit_width}) {}

    explicit scope_type(const state_type &state) { reset(state); }

    scope_type(const scope_type &) = delete;
    scope_type &operator=(const scope_type &) = delete;

    scope_type(scope_type &&other) noexcept
        : previous_width_(other.previous_width_),
          previous_active_(other.previous_active_),
          installed_(other.installed_) {
      other.installed_ = false;
    }

    scope_type &operator=(scope_type &&other) noexcept {
      if (this == &other)
        return *this;
      restore();
      previous_width_ = other.previous_width_;
      previous_active_ = other.previous_active_;
      installed_ = other.installed_;
      other.installed_ = false;
      return *this;
    }

    ~scope_type() { restore(); }

    void reset(unsigned bit_width) { reset(state_type{true, bit_width}); }

    void reset(const state_type &state) {
      restore();
      previous_width_ = current_bit_width_slot();
      previous_active_ = has_current_bit_width_slot();
      installed_ = true;
      if (state.active) {
        current_bit_width_slot() = state.bit_width;
        has_current_bit_width_slot() = true;
      } else {
        current_bit_width_slot() = 1;
        has_current_bit_width_slot() = false;
      }
    }

  private:
    void restore() {
      if (!installed_)
        return;
      current_bit_width_slot() = previous_width_;
      has_current_bit_width_slot() = previous_active_;
      installed_ = false;
    }

    unsigned previous_width_;
    bool previous_active_;
    bool installed_ = false;
  };

  static state_type capture() {
    return state_type{has_current_bit_width_slot(), current_bit_width_slot()};
  }

  static unsigned require(const char *message) {
    assert(has_current_bit_width_slot() && message);
    return current_bit_width_slot();
  }

private:
  static unsigned &current_bit_width_slot() {
    static thread_local unsigned width = 1;
    return width;
  }

  static bool &has_current_bit_width_slot() {
    static thread_local bool active = false;
    return active;
  }
};

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

template <class D>
inline bool valid_newton_delta(const DomVal<D> &f_nu, const DomVal<D> &nu,
                               const DomVal<D> &delta) {
  return domain_exact_equal<D>(D::combine(nu, delta), f_nu);
}

template <class D>
inline bool run_basic_domain_contract_checks(bool verbose = false) {
  bool ok = true;
  if (!D::equal(D::zero(), D::zero())) {
    ok = false;
    if (verbose)
      std::cerr << "[npa-contract] zero() must equal itself\n";
  }
  if (!D::equal(D::one(), D::one())) {
    ok = false;
    if (verbose)
      std::cerr << "[npa-contract] one() must equal itself\n";
  }
  if (D::idempotent) {
    if (!D::equal(D::combine(D::zero(), D::zero()), D::zero())) {
      ok = false;
      if (verbose)
        std::cerr << "[npa-contract] idempotent domain: zero⊕zero != zero\n";
    }
    if (!D::equal(D::combine(D::one(), D::one()), D::one())) {
      ok = false;
      if (verbose)
        std::cerr << "[npa-contract] idempotent domain: one⊕one != one\n";
    }
  }
  return ok;
}

class InvalidNewtonDeltaError : public std::logic_error {
public:
  InvalidNewtonDeltaError()
      : std::logic_error("invalid Newton delta: non-idempotent domains must "
                         "provide subtract()/choose_delta() such that "
                         "combine(nu, delta) == f(nu)") {}
};

class UnsupportedNewtonMuError : public std::logic_error {
public:
  UnsupportedNewtonMuError()
      : std::logic_error("unsupported Newton expression: Mu is evaluable but "
                         "outside the paper-faithful Newton/tensor fragment") {}
};

class UnsafeNewtonProjectError : public std::logic_error {
public:
  UnsafeNewtonProjectError()
      : std::logic_error(
            "unsafe Newton projection: domains must opt in with "
            "project_newton_safe for Project on Newton/tensor paths") {}
};

template <class D>
inline void require_valid_newton_delta(const DomVal<D> &f_nu,
                                       const DomVal<D> &nu,
                                       const DomVal<D> &delta) {
  // This check keeps the non-idempotent Newton hook honest: choose_delta() /
  // subtract() may be domain-specific, but they must still produce a residual
  // that exactly reconstructs f(nu) under combine().
  if (valid_newton_delta<D>(f_nu, nu, delta))
    return;
  throw InvalidNewtonDeltaError{};
}

} // namespace npa

#endif // NPA_RUNTIME_H
