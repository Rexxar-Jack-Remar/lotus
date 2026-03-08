#ifndef NPA_TENSOR_SEMIRING_H
#define NPA_TENSOR_SEMIRING_H

#include "Dataflow/NPA/Domains/TensorProductDomain.h"

namespace npa {

template <class D> struct TensorSemiringTraits;

namespace detail {
template <class D> struct TensorTraitsWellFormed {
private:
  template <class T>
  static auto test(int)
      -> typename std::enable_if<
          std::is_same<decltype(TensorSemiringTraits<T>::available()),
                       bool>::value &&
              std::is_same<
                  decltype(TensorSemiringTraits<T>::right_constant(
                      std::declval<const DomVal<T> &>())),
                  typename TensorSemiringTraits<T>::tensor_domain::value_type>::
                  value &&
              std::is_same<
                  decltype(TensorSemiringTraits<T>::left_constant(
                      std::declval<const DomVal<T> &>())),
                  typename TensorSemiringTraits<T>::tensor_domain::value_type>::
                  value &&
              std::is_same<
                  decltype(TensorSemiringTraits<T>::couple(
                      std::declval<const DomVal<T> &>(),
                      std::declval<const DomVal<T> &>())),
                  typename TensorSemiringTraits<T>::tensor_domain::value_type>::
                  value &&
              std::is_same<
                  decltype(TensorSemiringTraits<T>::readout(std::declval<
                           const typename TensorSemiringTraits<
                               T>::tensor_domain::value_type &>())),
                  DomVal<T>>::value,
          std::true_type>::type;
  template <class> static std::false_type test(...);

public:
  static constexpr bool value =
      std::is_same<decltype(test<D>(0)), std::true_type>::value;
};
} // namespace detail

template <class D> inline void validate_tensor_trait_api() {
  static_assert(detail::TensorTraitsWellFormed<D>::value,
                "TensorSemiringTraits<D> must define available(), "
                "right_constant(), left_constant(), couple(), and readout() "
                "with the expected tensor/base-domain types");
}

/// Tensor-side hook for TOPLAS-style regularization.
///
/// Domains can specialize this trait to provide:
/// - a tensor semiring (`tensor_domain`)
/// - right embedding of base constants (`right_constant`) for `1^t ⊙ c`
/// - left embedding of base constants (`left_constant`) for `c^t ⊙ 1`
/// - coupling of left/right coefficients (`couple`)
/// - readout/detensor operation (`readout`)
///
/// The default is unavailable: domains must opt in explicitly with a
/// specialization that defines the tensor-side semantics they want to use.
/// This keeps `LinearStrategy::TensorProduct` from silently assuming that the
/// generic paired utility domains are paper-faithful admissible semirings.
template <class D> struct TensorSemiringTraits {
  using tensor_domain = TensorProductExactDomain<D>;

  static bool available() { return false; }

  static typename tensor_domain::value_type right_constant(const DomVal<D> &v) {
    return domain_equal<D>(v, D::zero()) ? tensor_domain::zero()
                                         : tensor_domain::singleton(D::one(), v);
  }

  static typename tensor_domain::value_type left_constant(const DomVal<D> &v) {
    return domain_equal<D>(v, D::zero()) ? tensor_domain::zero()
                                         : tensor_domain::singleton(v, D::one());
  }

  static typename tensor_domain::value_type constant(const DomVal<D> &v) {
    return right_constant(v);
  }

  static typename tensor_domain::value_type couple(const DomVal<D> &lhs,
                                                   const DomVal<D> &rhs) {
    return tensor_domain::singleton(lhs, rhs);
  }

  static DomVal<D> readout(const typename tensor_domain::value_type &v) {
    return tensor_domain::project(v);
  }
};

template <class D>
inline typename TensorSemiringTraits<D>::tensor_domain::value_type
lift_base_value_to_tensor(const DomVal<D> &v) {
  return TensorSemiringTraits<D>::right_constant(v);
}

} // namespace npa

#endif // NPA_TENSOR_SEMIRING_H
