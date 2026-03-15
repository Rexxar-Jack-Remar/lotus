#ifndef NPA_COMMON_H
#define NPA_COMMON_H

/**
 * \file
 * \brief NPA common types and domain concept (semiring).
 *
 * Newtonian Program Analysis (NPA) solves systems of equations over
 * ω-continuous semirings. The framework expects a \e domain (semiring) D with:
 * - combine (⊕), extend (⊗), extend_lin (linearized equations), zero (⊥), one
 * (1)
 * - subtract is required only for non-idempotent domains
 *
 * References:
 * - Esparza et al., "Newtonian Program Analysis" (JACM): Newton sequence
 *   ν^(i+1) = ν^(i) + Δ^(i) where Δ^(i) is the least solution of the linearized
 *   system Df|ν^(i)(X) + δ^(i) = X.
 * - Reps et al., "Newtonian Program Analysis via Tensor Product" (TOPLAS 2016):
 *   Solving the LCFL linear sub-problems via tensor-product regularization.
 */

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <deque>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace npa {

using Symbol = std::string;

/// Strategy for solving the \e linearized equation system on each Newton round.
/// The linear system has the form Df|ν(X) + δ = X (LCFL equation system).
enum class LinearStrategy {
  /// Vector fixpoint: update all variables each round (chaotic iteration).
  Naive,
  /// Dependency-driven worklist (chaotic iteration with dependency graph).
  Worklist,
  /// SCC-based: Tarjan SCCs, solve in topological order, fixpoint per SCC.
  SCC,
  /// Tensor-product (TOPLAS 2016): lift LCFL system to paired semiring,
  /// solve as left-linear (regular) system, then project back. Only used when
  /// the linear system has LCFL structure (Concat/Star).
  TensorProduct
};

enum class DomainContractMode {
  Off,
  BasicChecks,
};

enum class IndirectCallResolutionMode {
  ClosedWorldTypeCompatible,
  DeclaredOnlyFallback,
  CustomResolverRequired,
};

template <class T> inline void hash_combine(std::size_t &h, const T &v) {
  h ^= std::hash<T>{}(v) + 0x9e3779b9 + (h << 6) + (h >> 2);
}

struct Stat {
  double time{};
  int iters{};
  bool converged = true;
  bool hit_limit = false;
  int equation_count = 0;
  int requested_max_iters = -1;
  int effective_max_iters = -1;
  LinearStrategy linear_strategy = LinearStrategy::Worklist;
  bool used_approx_equal = false;
  bool used_auto_n_cap = false;
  bool retried_without_auto_n_cap = false;
  bool domain_contract_checks_run = false;
  bool domain_contract_checks_failed = false;
};

struct AnalysisStatus {
  Stat summary_solve;
  long propagation_steps = 0;
  bool propagation_converged = true;
  bool propagation_hit_limit = false;
  bool configuration_error = false;
  bool unsupported_specs = false;
  bool approximated = false;
  bool overall_converged = true;
  bool overall_hit_limit = false;
  IndirectCallResolutionMode call_resolution_mode =
      IndirectCallResolutionMode::ClosedWorldTypeCompatible;
  long indirect_calls_seen = 0;
  long unresolved_indirect_calls = 0;
  long fallback_call_edges = 0;
  bool requires_external_callee_resolver = false;
  bool open_world_unsound_mode = true;
};

/**********************************************************************
 * Domain concept (ω-continuous semiring)
 *
 * Required: zero, one, combine (⊕), extend (⊗), extend_lin, ndetCombine,
 * condCombine, equal. See Esparza et al. for the semiring axioms and
 * ω-continuity; NPA uses the least fixed point μf of f. subtract() is
 * required only for non-idempotent domains.
 *********************************************************************/
template <class D> struct DomainHasBase {
  template <class T>
  static auto test(int)
      -> decltype(T::zero(), T::one(), T::combine(T::zero(), T::zero()),
                  T::extend(T::zero(), T::zero()),
                  T::extend_lin(T::zero(), T::zero()),
                  T::ndetCombine(T::zero(), T::zero()),
                  T::condCombine(typename T::test_type{}, T::zero(), T::zero()),
                  T::equal(T::zero(), T::zero()), std::true_type{});
  template <class> static std::false_type test(...);

public:
  static constexpr bool value =
      std::is_same<decltype(test<D>(0)), std::true_type>::value;
};

template <class D> struct DomainHasSubtract {
  template <class T>
  static auto test(int)
      -> decltype(T::subtract(T::zero(), T::zero()), std::true_type{});
  template <class> static std::false_type test(...);

public:
  static constexpr bool value =
      std::is_same<decltype(test<D>(0)), std::true_type>::value;
};

template <class D> struct DomainHasChooseDelta {
  template <class T>
  static auto test(int)
      -> decltype(T::choose_delta(T::zero(), T::zero()), std::true_type{});
  template <class> static std::false_type test(...);

public:
  static constexpr bool value =
      std::is_same<decltype(test<D>(0)), std::true_type>::value;
};

template <class D> struct DomainHasApproxEqual {
  template <class T>
  static auto test(int)
      -> decltype(T::approx_equal(T::zero(), T::zero()), std::true_type{});
  template <class> static std::false_type test(...);

public:
  static constexpr bool value =
      std::is_same<decltype(test<D>(0)), std::true_type>::value;
};

template <class D> struct DomainHasCommutativeExtend {
  template <class T>
  static auto test(int) -> decltype(T::commutative_extend, std::true_type{});
  template <class> static std::false_type test(...);

public:
  static constexpr bool value =
      std::is_same<decltype(test<D>(0)), std::true_type>::value;
};

template <class D> struct DomainHasProject {
  template <class T>
  static auto test(int) ->
      typename std::enable_if<std::is_same<decltype(T::project(T::zero())),
                                           typename T::value_type>::value,
                              std::true_type>::type;
  template <class> static std::false_type test(...);

public:
  static constexpr bool value =
      std::is_same<decltype(test<D>(0)), std::true_type>::value;
};

template <class D> struct DomainHasProjectT {
  template <class T>
  static auto test(int) ->
      typename std::enable_if<std::is_same<decltype(T::projectT(T::zero())),
                                           typename T::value_type>::value,
                              std::true_type>::type;
  template <class> static std::false_type test(...);

public:
  static constexpr bool value =
      std::is_same<decltype(test<D>(0)), std::true_type>::value;
};

template <class D> struct DomainHasProjectNewtonSafe {
  template <class T>
  static auto test(int) -> decltype(T::project_newton_safe, std::true_type{});
  template <class> static std::false_type test(...);

public:
  static constexpr bool value =
      std::is_same<decltype(test<D>(0)), std::true_type>::value;
};

template <class D> struct DomainHasMaxFixpointIters {
  template <class T>
  static auto test(int) -> decltype(T::max_fixpoint_iters, std::true_type{});
  template <class> static std::false_type test(...);

public:
  static constexpr bool value =
      std::is_same<decltype(test<D>(0)), std::true_type>::value;
};

template <class D> struct DomainHasMaxLinearSteps {
  template <class T>
  static auto test(int) -> decltype(T::max_linear_steps, std::true_type{});
  template <class> static std::false_type test(...);

public:
  static constexpr bool value =
      std::is_same<decltype(test<D>(0)), std::true_type>::value;
};

template <class D> using DomVal = typename D::value_type;
template <class D> using DomTest = typename D::test_type;

#define NPA_REQUIRE_DOMAIN(D)                                                  \
  static_assert(DomainHasBase<D>::value,                                       \
                "Invalid DOMAIN: missing required methods");                   \
  static_assert(                                                               \
      D::idempotent || DomainHasSubtract<D>::value ||                          \
          DomainHasChooseDelta<D>::value,                                      \
      "Non-idempotent DOMAIN must implement subtract() or choose_delta()")

struct Dirty {
  mutable bool dirty_ = true;
  void mark(bool d = true) const { dirty_ = d; }
};

template <class V> struct Optional {
  bool has{false};
  V val{};
  Optional() = default;
  Optional(const Optional &) = default;
  Optional &operator=(const Optional &) = default;
  Optional &operator=(const V &v_in) {
    val = v_in;
    has = true;
    return *this;
  }
  void reset() { has = false; }
  bool has_value() const { return has; }
  V &operator*() { return val; }
  const V &operator*() const { return val; }
};

namespace detail {
template <class D>
inline bool domain_equal_impl(const DomVal<D> &a, const DomVal<D> &b,
                              std::true_type) {
  return D::approx_equal(a, b);
}
template <class D>
inline bool domain_equal_impl(const DomVal<D> &a, const DomVal<D> &b,
                              std::false_type) {
  return D::equal(a, b);
}
} // namespace detail

/// Default equality used across solvers.
///
/// Paper-faithful domains should normally rely on exact semiring equality.
/// Domains may provide approx_equal() as a pragmatic extension for numeric
/// semirings where exact equality is too strong for convergence; once used,
/// stability/convergence checks become approximate rather than theorem-exact.
template <class D>
inline bool domain_equal(const DomVal<D> &a, const DomVal<D> &b) {
  return detail::domain_equal_impl<D>(
      a, b, std::integral_constant<bool, DomainHasApproxEqual<D>::value>{});
}

template <class D>
inline bool domain_exact_equal(const DomVal<D> &a, const DomVal<D> &b) {
  return D::equal(a, b);
}

/// Natural order for idempotent semirings: a ⊑ b  iff  a ⊕ b = b.
template <class D>
inline bool domain_leq_idempotent(const DomVal<D> &a, const DomVal<D> &b) {
  static_assert(D::idempotent,
                "domain_leq_idempotent requires an idempotent domain");
  return domain_equal<D>(D::combine(a, b), b);
}

namespace detail {
template <class D>
inline DomVal<D> domain_project_impl(const DomVal<D> &v, std::true_type,
                                     std::false_type) {
  return D::project(v);
}
template <class D>
inline DomVal<D> domain_project_impl(const DomVal<D> &v, std::false_type,
                                     std::true_type) {
  return D::projectT(v);
}
template <class D>
inline DomVal<D> domain_project_impl(const DomVal<D> &,
                                     std::false_type /* has_project */,
                                     std::false_type /* has_project_t */) {
  assert(false && "Domain must implement project() or projectT() to evaluate "
                  "projection expressions");
  return D::zero();
}
} // namespace detail

template <class D> inline DomVal<D> domain_project(const DomVal<D> &v) {
  return detail::domain_project_impl<D>(
      v, std::integral_constant<bool, DomainHasProject<D>::value>{},
      std::integral_constant<bool, DomainHasProjectT<D>::value>{});
}

namespace detail {
template <class D> inline bool domain_commutative_extend_impl(std::true_type) {
  return D::commutative_extend;
}
template <class D> inline bool domain_commutative_extend_impl(std::false_type) {
  return false;
}
template <class D> inline bool domain_project_newton_safe_impl(std::true_type) {
  return D::project_newton_safe;
}
template <class D>
inline bool domain_project_newton_safe_impl(std::false_type) {
  return false;
}
} // namespace detail

/// Returns true if D declares commutative_extend, otherwise false.
template <class D> inline bool domain_commutative_extend() {
  return detail::domain_commutative_extend_impl<D>(
      std::integral_constant<bool, DomainHasCommutativeExtend<D>::value>{});
}

template <class D> inline bool domain_project_newton_safe() {
  return detail::domain_project_newton_safe_impl<D>(
      std::integral_constant<bool, DomainHasProjectNewtonSafe<D>::value>{});
}

namespace detail {
template <class D> inline int domain_max_fixpoint_iters_impl(std::true_type) {
  return D::max_fixpoint_iters;
}
template <class D> inline int domain_max_fixpoint_iters_impl(std::false_type) {
  return -1;
}
template <class D> inline long domain_max_linear_steps_impl(std::true_type) {
  return static_cast<long>(D::max_linear_steps);
}
template <class D> inline long domain_max_linear_steps_impl(std::false_type) {
  return -1;
}
} // namespace detail

template <class D> inline int domain_max_fixpoint_iters() {
  return detail::domain_max_fixpoint_iters_impl<D>(
      std::integral_constant<bool, DomainHasMaxFixpointIters<D>::value>{});
}

template <class D> inline long domain_max_linear_steps() {
  return detail::domain_max_linear_steps_impl<D>(
      std::integral_constant<bool, DomainHasMaxLinearSteps<D>::value>{});
}

inline bool &npa_limit_hit_flag() {
  static thread_local bool flag = false;
  return flag;
}

inline void npa_reset_limit_hit() { npa_limit_hit_flag() = false; }

inline void npa_note_limit_hit() { npa_limit_hit_flag() = true; }

inline bool npa_limit_hit() { return npa_limit_hit_flag(); }

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

#endif // NPA_COMMON_H
