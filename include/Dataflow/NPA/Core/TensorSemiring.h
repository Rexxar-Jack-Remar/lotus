#ifndef NPA_TENSOR_SEMIRING_H
#define NPA_TENSOR_SEMIRING_H

#include "Dataflow/NPA/Domains/TensorProductDomain.h"

namespace npa {

/// Tensor-side hook for TOPLAS-style regularization.
///
/// Domains can specialize this trait to provide:
/// - a tensor semiring (`tensor_domain`)
/// - embedding of base constants into tensor space (`constant`)
/// - coupling of left/right coefficients (`couple`)
/// - readout/detensor operation (`readout`)
///
/// The default is unavailable: domains must opt in explicitly with a
/// specialization that defines the tensor-side semantics they want to use.
/// This keeps `LinearStrategy::TensorProduct` aligned with paper-specific
/// admissible semiring constructions instead of silently applying a fallback.
template <class D> struct TensorSemiringTraits {
  using tensor_domain = TensorProductExactDomain<D>;

  static bool available() { return false; }

  static typename tensor_domain::value_type constant(const DomVal<D> &v) {
    return domain_equal<D>(v, D::zero()) ? tensor_domain::zero()
                                         : tensor_domain::singleton(v, D::one());
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
  return TensorSemiringTraits<D>::constant(v);
}

} // namespace npa

#endif // NPA_TENSOR_SEMIRING_H
