#ifndef NPA_FOUNDATION_H
#define NPA_FOUNDATION_H

/**
 * \file
 * \brief Public NPA types plus generic semiring/domain support helpers.
 */

#include <cassert>
#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>

namespace npa {

using Symbol = std::string;

/// Strategy for solving the \e linearized equation system on each Newton round.
/// The linear system has the form Df|ν(X) + δ = X (LCFL equation system).
enum class LinearStrategy {
  Naive,
  SCC,
  AdaptiveScc,
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
  bool hit_outer_limit = false;
  bool hit_linear_limit = false;
  bool hit_fixpoint_limit = false;
  int equation_count = 0;
  int requested_max_iters = -1;
  int effective_max_iters = -1;
  LinearStrategy linear_strategy = LinearStrategy::SCC;
  bool used_approx_equal = false;
  bool used_auto_n_cap = false;
  bool retried_without_auto_n_cap = false;
  bool adaptive_scc_used = false;
  int adaptive_scc_direct_count = 0;
  int adaptive_scc_worklist_count = 0;
  int adaptive_scc_tensor_count = 0;
  int adaptive_scc_tensor_fallback_count = 0;
  bool domain_contract_checks_run = false;
  bool domain_contract_checks_failed = false;
};

struct AnalysisStatus {
  Stat summary_solve;
  double phase_artifact_construction_time = 0.0;
  double phase_summary_materialization_time = 0.0;
  double phase_propagation_time = 0.0;
  long propagation_steps = 0;
  bool propagation_converged = true;
  bool propagation_hit_limit = false;
  bool configuration_error = false;
  bool unsupported_specs = false;
  bool approximated = false;
  bool used_summary_overflow = false;
  bool used_fact_widening = false;
  bool used_bounded_inner_solve = false;
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

template <class V> using Optional = std::optional<V>;

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

} // namespace npa

#endif // NPA_FOUNDATION_H
