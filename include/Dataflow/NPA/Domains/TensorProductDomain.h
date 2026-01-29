#ifndef NPA_TENSOR_PRODUCT_DOMAIN_H
#define NPA_TENSOR_PRODUCT_DOMAIN_H

#include <utility>

namespace npa {

/**
 * TensorProductDomain<D> – Tensor product of semiring D with itself
 * (TOPLAS 2016: Newtonian Program Analysis via Tensor Product)
 *
 * Value type: pair (v_L, v_R) representing "left" and "right" context.
 * Used to convert LCFL (two-sided) linear systems into regular (one-sided)
 * systems in the tensor space.
 *
 * Semiring structure (standard tensor product):
 *   zero  = (0, 0)
 *   one   = (1, 1)
 *   combine((a,b), (c,d)) = (combine(a,c), combine(b,d))
 *   extend((a,b), (c,d))  = (extend(a,c), extend(b,d))
 *
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
    return {D::extend(a.first, b.first), D::extend(a.second, b.second)};
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

  /** Project tensor value back to base domain (e.g. left component). */
  static V project_left(const value_type& p) { return p.first; }
  static V project_right(const value_type& p) { return p.second; }
  /** For idempotent semirings, left and right coincide at fixpoint; use left. */
  static V project(const value_type& p) { return p.first; }
};

} // namespace npa

#endif
