#ifndef NPA_TENSOR_PRODUCT_DOMAIN_H
#define NPA_TENSOR_PRODUCT_DOMAIN_H

#include <utility>

namespace npa {

/**
 * TensorProductDomain<D> – Paired semiring for LCFL regularization
 * (Reps et al., "Newtonian Program Analysis via Tensor Product", TOPLAS 2016).
 *
 * Value type: pair (v_L, v_R) representing left and right context. Used to
 * convert an LCFL equation system Y_j = c_j ⊕ ⊕_{i,k} (a_{i,j,k} ⊗ Y_i ⊗
 * b_{i,j,k}) into a \e left-linear system over pairs (Alg. 3.4). Pairing
 * accumulates left/right coefficients separately so that the readout
 * R((w1,w2)) = w1 ⊗ w2 recovers the desired mirrored correlation.
 *
 * Paper notation: (a1,b1) ⊗_p (a2,b2) = (a2⊗a1, b1⊗b2); ⊕_p componentwise;
 * R((a,b)) = a⊗b. extend((a,b), (c,d)) implements ⊗_p so that
 * R(extend(p1,p2)) = R(p2)⊗R(p1) for the intended matching.
 *
 * Semiring: zero = (0,0), one = (1,1), combine = ⊕_p, extend = ⊗_p.
 * Idempotent and subtract follow from D.
 */
template <class D>
struct TensorProductDomain {
  using V = typename D::value_type;
  using value_type = std::pair<V, V>;
  using test_type = typename D::test_type;
  static constexpr bool idempotent = D::idempotent;

  static value_type zero() {
    return {D::zero(), D::zero()};
  }
  static value_type one() {
    return {D::one(), D::one()};
  }
  static bool equal(const value_type& a, const value_type& b) {
    return D::equal(a.first, b.first) && D::equal(a.second, b.second);
  }
  static value_type combine(const value_type& a, const value_type& b) {
    return {D::combine(a.first, b.first), D::combine(a.second, b.second)};
  }
  static value_type extend(const value_type& a, const value_type& b) {
    // Paper: (a1,b1) ⊗_p (a2,b2) = (a2⊗a1, b1⊗b2). So extend(a,b) with a=(a1,b1),
    // b=(a2,b2) must yield (a2⊗a1, b1⊗b2) so that R(extend(p1,p2))=R(p2)⊗R(p1).
    return {D::extend(b.first, a.first), D::extend(a.second, b.second)};
  }
  static value_type extend_lin(const value_type& a, const value_type& b) {
    return extend(a, b);
  }
  static value_type ndetCombine(const value_type& a, const value_type& b) {
    return combine(a, b);
  }
  static value_type condCombine(test_type phi, const value_type& t,
                                 const value_type& e) {
    return phi ? t : e;
  }
  static value_type subtract(const value_type& a, const value_type& b) {
    return {D::subtract(a.first, b.first), D::subtract(a.second, b.second)};
  }

  /** Project tensor value back to base domain (readout R((w1,w2)) = w1⊗w2). */
  static V project_left(const value_type& p) { return p.first; }
  static V project_right(const value_type& p) { return p.second; }
  /** Readout R((a,b)) = a⊗b (Reps et al. Alg. 3.4). */
  static V project(const value_type& p) {
    return D::extend(p.first, p.second);
  }
};

} // namespace npa

#endif
