#ifndef NPA_TENSOR_DIFF_H
#define NPA_TENSOR_DIFF_H

#include "Dataflow/NPA/Core/Expressions.h"
#include "Dataflow/NPA/Core/TensorSemiring.h"

namespace npa {

/// Direct tensor-side differential builder. This exposes a first-class tensor
/// differential API instead of forcing callers to build the ordinary
/// differential and convert it afterwards.
template <class D> struct TensorDiff {
  using V = DomVal<D>;
  using Traits = TensorSemiringTraits<D>;
  using TD = typename Traits::tensor_domain;
  using Map = std::unordered_map<Symbol, V>;

  static E1<TD> build(const Map &nu, const E0<D> &e) { return aux(nu, e); }

private:
  static E1<TD> aux(const Map &nu, const E0<D> &o) {
    using K0 = typename Exp0<D>::K;
    switch (o->k) {
    case K0::Term:
      return Exp1<TD>::term(TD::zero());
    case K0::Seq:
      return Exp1<TD>::seqR(aux(nu, o->t), Traits::constant(o->c));
    case K0::Mul: {
      assert(o->t1->val.has_value() && o->t2->val.has_value());
      auto lhs = aux(nu, o->t1);
      auto rhs = aux(nu, o->t2);
      return Exp1<TD>::add(Exp1<TD>::seqR(lhs, Traits::constant(*o->t2->val)),
                           Exp1<TD>::seq(Traits::constant(*o->t1->val), rhs));
    }
    case K0::Call: {
      auto dArg = aux(nu, o->t);
      auto left = Exp1<TD>::seqR(dArg, Traits::constant(nu.at(o->sym)));
      assert(o->t->val.has_value());
      auto right = Exp1<TD>::seq(Traits::constant(*o->t->val),
                                 Exp1<TD>::hole(o->sym));
      return Exp1<TD>::add(left, right);
    }
    case K0::Cond:
      return Exp1<TD>::cond(o->phi, aux(nu, o->t1), aux(nu, o->t2));
    case K0::Ndet:
      return Exp1<TD>::add(aux(nu, o->t1), aux(nu, o->t2));
    case K0::Hole:
      return Exp1<TD>::hole(o->sym);
    case K0::Bound:
      return Exp1<TD>::term(TD::zero());
    case K0::Concat: {
      assert(o->t1->val.has_value() && o->t2->val.has_value());
      V t1_val = *o->t1->val;
      V t2_val = *o->t2->val;
      V nu_x = nu.at(o->sym);
      auto d1 = aux(nu, o->t1);
      auto d2 = aux(nu, o->t2);
      auto term1 =
          Exp1<TD>::seqR(d1, Traits::constant(D::extend(nu_x, t2_val)));
      auto term2 =
          Exp1<TD>::seqR(Exp1<TD>::hole(o->sym), Traits::couple(t1_val, t2_val));
      auto term3 = Exp1<TD>::seqR(Exp1<TD>::seqR(d2, Traits::constant(nu_x)),
                                  Traits::constant(t1_val));
      return Exp1<TD>::add(Exp1<TD>::add(term1, term2), term3);
    }
    case K0::InfClos:
      // TOPLAS 2016, Section 6.2: D^T(g*) = D^T(g) ⊗T ((g(ν)*)^t ⊙ g(ν)*).
      assert(o->val.has_value());
      return Exp1<TD>::seqR(aux(nu, o->t),
                            Traits::couple(*o->val, *o->val));
    }
    return nullptr;
  }
};

} // namespace npa

#endif // NPA_TENSOR_DIFF_H
