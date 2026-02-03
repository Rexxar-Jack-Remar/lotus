#ifndef NPA_EXPRESSIONS_H
#define NPA_EXPRESSIONS_H

#include "Dataflow/NPA/Core/NPACommon.h"

namespace npa {

template <class D>
struct Exp0;
template <class D>
using E0 = std::shared_ptr<Exp0<D>>;

template <class D>
struct Exp0 : Dirty, std::enable_shared_from_this<Exp0<D>> {
  using V = DomVal<D>;
  using T = DomTest<D>;
  enum K { Term, Seq, Call, Cond, Ndet, Hole, Bound, Concat, InfClos };
  K k;
  V c;
  E0<D> t;
  Symbol sym;
  T phi;
  E0<D> t1, t2;
  mutable Optional<V> val;
  static E0<D> term(V v) {
    auto e = std::make_shared<Exp0>();
    e->k = Term;
    e->c = v;
    return e;
  }
  static E0<D> seq(V c, E0<D> t) {
    auto e = std::make_shared<Exp0>();
    e->k = Seq;
    e->c = c;
    e->t = t;
    return e;
  }
  static E0<D> call(Symbol f, E0<D> arg) {
    auto e = std::make_shared<Exp0>();
    e->k = Call;
    e->sym = f;
    e->t = arg;
    return e;
  }
  static E0<D> cond(T phi, E0<D> t_then, E0<D> t_else) {
    auto e = std::make_shared<Exp0>();
    e->k = Cond;
    e->phi = phi;
    e->t1 = t_then;
    e->t2 = t_else;
    return e;
  }
  static E0<D> ndet(E0<D> a, E0<D> b) {
    auto e = std::make_shared<Exp0>();
    e->k = Ndet;
    e->t1 = a;
    e->t2 = b;
    return e;
  }
  static E0<D> hole(Symbol x) {
    auto e = std::make_shared<Exp0>();
    e->k = Hole;
    e->sym = x;
    return e;
  }
  static E0<D> var(Symbol x) { return hole(std::move(x)); }
  static E0<D> bound(Symbol x) {
    auto e = std::make_shared<Exp0>();
    e->k = Bound;
    e->sym = std::move(x);
    return e;
  }
  static E0<D> concat(E0<D> a, Symbol x, E0<D> b) {
    auto e = std::make_shared<Exp0>();
    e->k = Concat;
    e->t1 = a;
    e->t2 = b;
    e->sym = x;
    return e;
  }
  static E0<D> inf(E0<D> body, Symbol x) {
    auto e = std::make_shared<Exp0>();
    e->k = InfClos;
    e->t = body;
    e->sym = x;
    return e;
  }
};

template <class D>
struct Exp1;
template <class D>
using E1 = std::shared_ptr<Exp1<D>>;

template <class D>
struct Exp1 : Dirty {
  using V = DomVal<D>;
  using T = DomTest<D>;
  enum K { Term, Seq, Call, Cond, Ndet, Hole, Bound, Concat, InfClos, Add, Sub };
  K k;
  V c;
  Symbol sym;
  T phi;
  E1<D> t, t1, t2;
  mutable Optional<V> val;
  static E1<D> term(V v) {
    auto e = std::make_shared<Exp1>();
    e->k = Term;
    e->c = v;
    return e;
  }
  static E1<D> add(E1<D> a, E1<D> b) {
    auto e = std::make_shared<Exp1>();
    e->k = Add;
    e->t1 = a;
    e->t2 = b;
    return e;
  }
  static E1<D> sub(E1<D> a, E1<D> b) {
    auto e = std::make_shared<Exp1>();
    e->k = Sub;
    e->t1 = a;
    e->t2 = b;
    return e;
  }
  static E1<D> seq(V c, E1<D> t) {
    auto e = std::make_shared<Exp1>();
    e->k = Seq;
    e->c = c;
    e->t = t;
    return e;
  }
  static E1<D> call(Symbol f, V c) {
    auto e = std::make_shared<Exp1>();
    e->k = Call;
    e->sym = f;
    e->c = c;
    return e;
  }
  static E1<D> cond(T phi, E1<D> t_then, E1<D> t_else) {
    auto e = std::make_shared<Exp1>();
    e->k = Cond;
    e->phi = phi;
    e->t1 = t_then;
    e->t2 = t_else;
    return e;
  }
  static E1<D> ndet(E1<D> a, E1<D> b) {
    auto e = std::make_shared<Exp1>();
    e->k = Ndet;
    e->t1 = a;
    e->t2 = b;
    return e;
  }
  static E1<D> hole(Symbol x) {
    auto e = std::make_shared<Exp1>();
    e->k = Hole;
    e->sym = x;
    return e;
  }
  static E1<D> var(Symbol x) { return hole(std::move(x)); }
  static E1<D> bound(Symbol x) {
    auto e = std::make_shared<Exp1>();
    e->k = Bound;
    e->sym = std::move(x);
    return e;
  }
  static E1<D> concat(E1<D> a, Symbol x, E1<D> b) {
    auto e = std::make_shared<Exp1>();
    e->k = Concat;
    e->t1 = a;
    e->t2 = b;
    e->sym = x;
    return e;
  }
  static E1<D> inf(E1<D> body, Symbol x) {
    auto e = std::make_shared<Exp1>();
    e->k = InfClos;
    e->t = body;
    e->sym = x;
    return e;
  }
};

template <class D>
struct DepFinder {
  using Set = std::unordered_set<Symbol>;
  static void find(const E1<D> &e, Set &deps) {
    if (!e) return;
    using K = typename Exp1<D>::K;
    switch (e->k) {
    case K::Hole:
      deps.insert(e->sym);
      break;
    case K::Bound:
      break;
    case K::Call:
      deps.insert(e->sym);
      break;
    case K::Concat:
      deps.insert(e->sym);
      find(e->t1, deps);
      find(e->t2, deps);
      break;
    case K::InfClos:
      deps.insert(e->sym);
      find(e->t, deps);
      break;
    default:
      if (e->t) find(e->t, deps);
      if (e->t1) find(e->t1, deps);
      if (e->t2) find(e->t2, deps);
      break;
    }
  }
};

} // namespace npa

#endif // NPA_EXPRESSIONS_H
