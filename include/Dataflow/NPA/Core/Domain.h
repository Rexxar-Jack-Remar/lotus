#ifndef NPA_CORE_DOMAIN_H
#define NPA_CORE_DOMAIN_H

/**
 * \file
 * \brief Domain concept detection and solver-independent domain operations.
 */

#include <cassert>
#include <optional>
#include <type_traits>

namespace npa {

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
template <class V> using Optional = std::optional<V>;

#define NPA_REQUIRE_DOMAIN(D)                                                  \
  static_assert(DomainHasBase<D>::value,                                      \
                "Invalid DOMAIN: missing required methods");                  \
  static_assert(                                                              \
      D::idempotent || DomainHasSubtract<D>::value ||                         \
          DomainHasChooseDelta<D>::value,                                     \
      "Non-idempotent DOMAIN must implement subtract() or choose_delta()")

namespace detail {
template <class D>
inline bool domain_equal_impl(const DomVal<D> &lhs, const DomVal<D> &rhs,
                              std::true_type) {
  return D::approx_equal(lhs, rhs);
}

template <class D>
inline bool domain_equal_impl(const DomVal<D> &lhs, const DomVal<D> &rhs,
                              std::false_type) {
  return D::equal(lhs, rhs);
}
} // namespace detail

template <class D>
inline bool domain_equal(const DomVal<D> &lhs, const DomVal<D> &rhs) {
  return detail::domain_equal_impl<D>(
      lhs, rhs,
      std::integral_constant<bool, DomainHasApproxEqual<D>::value>{});
}

template <class D>
inline bool domain_exact_equal(const DomVal<D> &lhs, const DomVal<D> &rhs) {
  return D::equal(lhs, rhs);
}

template <class D>
inline bool domain_leq_idempotent(const DomVal<D> &lhs,
                                  const DomVal<D> &rhs) {
  static_assert(D::idempotent,
                "domain_leq_idempotent requires an idempotent domain");
  return domain_equal<D>(D::combine(lhs, rhs), rhs);
}

namespace detail {
template <class D>
inline DomVal<D> domain_project_impl(const DomVal<D> &value, std::true_type,
                                     std::false_type) {
  return D::project(value);
}

template <class D>
inline DomVal<D> domain_project_impl(const DomVal<D> &value, std::false_type,
                                     std::true_type) {
  return D::projectT(value);
}

template <class D>
inline DomVal<D> domain_project_impl(const DomVal<D> &, std::false_type,
                                     std::false_type) {
  assert(false && "Domain must implement project() or projectT() to evaluate "
                  "projection expressions");
  return D::zero();
}

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

template <class D> inline DomVal<D> domain_project(const DomVal<D> &value) {
  return detail::domain_project_impl<D>(
      value, std::integral_constant<bool, DomainHasProject<D>::value>{},
      std::integral_constant<bool, DomainHasProjectT<D>::value>{});
}

template <class D> inline bool domain_commutative_extend() {
  return detail::domain_commutative_extend_impl<D>(
      std::integral_constant<bool, DomainHasCommutativeExtend<D>::value>{});
}

template <class D> inline bool domain_project_newton_safe() {
  return detail::domain_project_newton_safe_impl<D>(
      std::integral_constant<bool, DomainHasProjectNewtonSafe<D>::value>{});
}

template <class D> inline int domain_max_fixpoint_iters() {
  return detail::domain_max_fixpoint_iters_impl<D>(
      std::integral_constant<bool, DomainHasMaxFixpointIters<D>::value>{});
}

template <class D> inline long domain_max_linear_steps() {
  return detail::domain_max_linear_steps_impl<D>(
      std::integral_constant<bool, DomainHasMaxLinearSteps<D>::value>{});
}

} // namespace npa

#endif // NPA_CORE_DOMAIN_H
