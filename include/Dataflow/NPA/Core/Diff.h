#ifndef NPA_DIFF_H
#define NPA_DIFF_H

#include "Dataflow/NPA/Core/Eval.h"

namespace npa {

template <class D>
struct Diff {
  using V = DomVal<D>;
  using M0 = E0<D>;
  using M1 = E1<D>;
  using Map = std::unordered_map<Symbol, V>;
  static M1 build(const Map &nu, const M0 &e) { return aux(nu, e, clone(e)); }

private:
  static M0 clone(const M0 &e) {
    auto c = std::make_shared<Exp0<D>>(*e);
    c->val.reset();
    c->dirty_ = true;
    if (e->t) c->t = clone(e->t);
    if (e->t1) c->t1 = clone(e->t1);
    if (e->t2) c->t2 = clone(e->t2);
    return c;
  }
  static M1 aux(const Map &nu, const M0 &o, const M0 &cur) {
    using K0 = typename Exp0<D>::K;
    switch (o->k) {
    case K0::Term:
      return Exp1<D>::term(D::zero());
    case K0::Seq:
      return Exp1<D>::seq(o->c, aux(nu, o->t, cur->t));
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
    case K0::Ndet: {
      auto a1 = aux(nu, o->t1, cur->t1), a2 = aux(nu, o->t2, cur->t2);
      assert(o->t1->val.has_value() && o->t2->val.has_value() &&
             o->val.has_value());
      auto aug1 = Exp1<D>::add(Exp1<D>::term(*o->t1->val), a1);
      auto aug2 = Exp1<D>::add(Exp1<D>::term(*o->t2->val), a2);
      auto augmented = Exp1<D>::ndet(aug1, aug2);
      return D::idempotent ? augmented
                           : Exp1<D>::sub(augmented, Exp1<D>::term(*o->val));
    }
    case K0::Hole:
      return Exp1<D>::hole(o->sym);
    case K0::Bound:
      // Bound variables are local to Concat/InfClos; treat them as constants
      // w.r.t. the system variables.
      return Exp1<D>::term(D::zero());
    case K0::Concat:
      return Exp1<D>::concat(aux(nu, o->t1, cur->t1), o->sym,
                             aux(nu, o->t2, cur->t2));
    case K0::InfClos:
      return Exp1<D>::inf(aux(nu, o->t, cur->t), o->sym);
    }
    return nullptr;
  }
};

} // namespace npa

#endif // NPA_DIFF_H
