#ifndef NPA_DIFF_H
#define NPA_DIFF_H

/**
 * \file
 * \brief Differential construction: builds linearized expression Df|ν from f.
 *
 * Given the current approximation ν and a polynomial expression e (one
 * equation of f), build the \e differential expression that corresponds to
 * Df|ν. The result is an Exp1 (linear in the variables) such that the
 * Newton round solves Df|ν(Δ) + δ = Δ, i.e. Δ^(i) is the least solution of
 * the linearized system (Esparza et al. JACM, Defn. 3.1, 3.5).
 *
 * Rules: Term -> 0; Seq -> c·d(t); Call -> ν(f)·d(arg) + f(ν(arg)); Cond -> by
 * branch; Ndet -> D(t1) ⊕ D(t2) (Defn 3.1 sum); Hole -> X; Bound -> 0;
 * Concat -> D(t1)·ν_X·t2 + t1·X·t2 + t1·ν_X·D(t2); InfClos -> g(ν)*·D(g)·g(ν)*
 * (TOPLAS 2016, Eq. (60)).
 */

#include "Dataflow/NPA/Core/Expressions.h"

namespace npa {

template <class D> struct Diff {
  using V = DomVal<D>;
  using M0 = E0<D>;
  using M1 = E1<D>;
  using Map = std::unordered_map<Symbol, V>;
  /// Build the differential of e at ν; requires e to be evaluated (val set).
  static M1 build(const Map &nu, const M0 &e) { return aux(nu, e, clone(e)); }

private:
  static M0 clone(const M0 &e) {
    auto c = std::make_shared<Exp0<D>>(*e);
    c->val.reset();
    c->dirty_ = true;
    if (e->t)
      c->t = clone(e->t);
    if (e->t1)
      c->t1 = clone(e->t1);
    if (e->t2)
      c->t2 = clone(e->t2);
    return c;
  }
  static M1 aux(const Map &nu, const M0 &o, const M0 &cur) {
    using K0 = typename Exp0<D>::K;
    switch (o->k) {
    case K0::Term:
      return Exp1<D>::term(D::zero());
    case K0::Seq:
      return Exp1<D>::seq(o->c, aux(nu, o->t, cur->t));
    case K0::Mul: {
      assert(o->t1->val.has_value() && o->t2->val.has_value());
      M1 lhs = aux(nu, o->t1, cur->t1);
      M1 rhs = aux(nu, o->t2, cur->t2);
      return Exp1<D>::add(Exp1<D>::seqR(lhs, *o->t2->val),
                          Exp1<D>::seq(*o->t1->val, rhs));
    }
    case K0::Call: {
      auto dArg = aux(nu, o->t, cur->t);
      auto left = Exp1<D>::seq(nu.at(o->sym), dArg);
      assert(o->t->val.has_value());
      auto right = Exp1<D>::call(o->sym, *o->t->val);
      return Exp1<D>::add(left, right);
    }
    case K0::Cond:
      return Exp1<D>::cond(o->phi, aux(nu, o->t1, cur->t1),
                           aux(nu, o->t2, cur->t2));
    case K0::Ndet:
      // Paper Defn 3.1: D(∑ f_i)|ν(b) = ∑ Df_i|ν(b). So D(ndet(t1,t2)) = D(t1)
      // ⊕ D(t2).
      return Exp1<D>::add(aux(nu, o->t1, cur->t1), aux(nu, o->t2, cur->t2));
    case K0::Hole:
      return Exp1<D>::hole(o->sym);
    case K0::Bound:
      // Bound variables are local to Concat/InfClos; treat them as constants
      // w.r.t. the system variables.
      return Exp1<D>::term(D::zero());
    case K0::Concat: {
      // Product rule: D(t1·X·t2)|ν(b) = D(t1)·ν_X·t2 + t1·b·t2 + t1·ν_X·D(t2)
      // (Esparza et al. Defn. 3.1: D(g·h) = D(g)·h(v) + g(v)·D(h)).
      assert(o->t1->val.has_value() && o->t2->val.has_value());
      V t1_val = *o->t1->val, t2_val = *o->t2->val;
      V nu_x = nu.at(o->sym);
      M1 d1 = aux(nu, o->t1, cur->t1), d2 = aux(nu, o->t2, cur->t2);
      M1 term1 = Exp1<D>::seqR(d1, D::extend(nu_x, t2_val));
      M1 term2 =
          Exp1<D>::concat(Exp1<D>::term(t1_val), o->sym, Exp1<D>::term(t2_val));
      M1 term3 = Exp1<D>::seq(t1_val, Exp1<D>::seq(nu_x, d2));
      return Exp1<D>::add(Exp1<D>::add(term1, term2), term3);
    }
    case K0::InfClos: {
      // TOPLAS 2016, Eq. (60): D(g*) = g(ν)* · D(g) · g(ν)*.
      assert(o->val.has_value());
      V star_val = *o->val;
      M1 body_diff = aux(nu, o->t, cur->t);
      return Exp1<D>::seq(star_val, Exp1<D>::seqR(body_diff, star_val));
    }
    }
    return nullptr;
  }
};

} // namespace npa

#endif // NPA_DIFF_H
