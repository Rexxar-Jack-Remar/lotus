#ifndef NPA_EXPRESSIONS_H
#define NPA_EXPRESSIONS_H

/**
 * \file
 * \brief Expression AST for polynomial (Exp0) and linearized (Exp1) equation
 * systems.
 *
 * Interprocedural dataflow is formulated as X = f(X) over a semiring. NPA
 * linearizes f at the current approximation ν to obtain a system of \e linear
 * equations; when multiplication is non-commutative, that system has the form
 * of an \e LCFL equation system (linear context-free language).
 *
 * - \e Exp0: polynomial expressions (terms, seq, call, cond, ndet, hole/bound,
 *   concat, inf-closure). Used for the full system f(X).
 * - \e Exp1: linearized expressions; adds Add/Sub for the differential form.
 *   Used for the right-hand side of Df|ν(X) + δ = X. Concat (t1·X·t2) and
 *   InfClos correspond to LCFL structure (coefficients on both sides of X).
 *
 * References:
 * - Esparza et al. (JACM): differential Df|ν; linearized system.
 * - Reps et al. (TOPLAS 2016): LCFL equation system Y_j = c_j ⊕ ⊕_{i,k}
 *   (a_{i,j,k} ⊗ Y_i ⊗ b_{i,j,k}); Concat encodes a·Y·b.
 */

#include "Dataflow/NPA/Core/NPACommon.h"

namespace npa {

template <class D> struct Exp0;
template <class D> using E0 = std::shared_ptr<Exp0<D>>;

/// Polynomial expression (full equation system f(X)).
/// Kinds: Term (constant), Seq (c·t), Call (procedure call), Cond, Ndet,
/// Hole/Bound (variable), Concat (t1·X·t2, LCFL form), InfClos (Kleene star).
template <class D> struct Exp0 : Dirty, std::enable_shared_from_this<Exp0<D>> {
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
  /// LCFL form: a·X·b (left coeff a, variable X, right coeff b). In the
  /// linearized system this yields terms a⊗Y⊗b that cannot be rearranged
  /// when extend is non-commutative (Reps et al. TOPLAS 2016, Defn. 3.1).
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

template <class D> struct Exp1;
template <class D> using E1 = std::shared_ptr<Exp1<D>>;

/// Linearized expression (right-hand side of Df|ν(X) + δ = X). Adds Add/Sub
/// for combine and differential; Concat/InfClos preserved from Exp0 (LCFL).
template <class D> struct Exp1 : Dirty {
  using V = DomVal<D>;
  using T = DomTest<D>;
  enum K {
    Term,
    Seq,
    SeqR,
    Call,
    Cond,
    Ndet,
    Hole,
    Bound,
    Concat,
    InfClos,
    Add,
    Sub
  };
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
  /// Right sequence: expr · constant (for differential D(expr)·constant).
  static E1<D> seqR(E1<D> t, V c) {
    auto e = std::make_shared<Exp1>();
    e->k = SeqR;
    e->t = t;
    e->c = c;
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

/// Collects variable symbols on which a linearized expression depends
/// (Hole, Call, Concat, InfClos). Used for worklist and dependency graph.
template <class D> struct DepFinder {
  using Set = std::unordered_set<Symbol>;
  static void find(const E1<D> &e, Set &deps) {
    if (!e)
      return;
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
    case K::SeqR:
      find(e->t, deps);
      break;
    default:
      if (e->t)
        find(e->t, deps);
      if (e->t1)
        find(e->t1, deps);
      if (e->t2)
        find(e->t2, deps);
      break;
    }
  }
};

template <class D> struct ExprFeatureDetector {
  static bool has_infclos(const E0<D> &e) {
    if (!e)
      return false;
    switch (e->k) {
    case Exp0<D>::InfClos:
      return true;
    case Exp0<D>::Seq:
    case Exp0<D>::Call:
      return has_infclos(e->t);
    case Exp0<D>::Cond:
    case Exp0<D>::Ndet:
    case Exp0<D>::Concat:
      return has_infclos(e->t1) || has_infclos(e->t2);
    default:
      return false;
    }
  }

  static bool has_infclos(const E1<D> &e) {
    if (!e)
      return false;
    using K = typename Exp1<D>::K;
    switch (e->k) {
    case K::InfClos:
      return true;
    case K::Seq:
    case K::SeqR:
      return has_infclos(e->t);
    case K::Cond:
    case K::Add:
    case K::Sub:
    case K::Ndet:
    case K::Concat:
      return has_infclos(e->t1) || has_infclos(e->t2);
    default:
      return false;
    }
  }
};

} // namespace npa

#endif // NPA_EXPRESSIONS_H
