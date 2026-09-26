#pragma once

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
 *   concat, star, mu). Used for the full system f(X).
 * - \e Exp1: linearized expressions; adds Add/Sub for the differential form.
 *   Used for the right-hand side of Df|ν(X) + δ = X. Concat (t1·X·t2) and
 *   Star correspond to LCFL structure (coefficients on both sides of X).
 *
 * References:
 * - Esparza et al. (JACM): differential Df|ν; linearized system.
 * - Reps et al. (TOPLAS 2016): LCFL equation system Y_j = c_j ⊕ ⊕_{i,k}
 *   (a_{i,j,k} ⊗ Y_i ⊗ b_{i,j,k}); Concat encodes a·Y·b.
 */

#include "Dataflow/NPA/Core/Domain.h"
#include "Dataflow/NPA/Core/Symbol.h"

#include <memory>
#include <unordered_set>

namespace npa {

template <class D> struct Exp0;
template <class D> using E0 = std::shared_ptr<Exp0<D>>;

/// Polynomial expression (full equation system f(X)).
/// Kinds: Term (constant), Seq (c·t), Mul (t1·t2), Call (procedure call),
/// Cond, Ndet, Project, Hole/Bound (variable), Concat (t1·X·t2, LCFL form),
/// Star (Kleene star), Mu (generic least fixpoint).
template <class D> struct Exp0 : std::enable_shared_from_this<Exp0<D>> {
  using V = DomVal<D>;
  using T = DomTest<D>;
  enum K {
    Term,
    Seq,
    Mul,
    Call,
    Cond,
    Ndet,
    Project,
    Hole,
    Bound,
    Concat,
    Star,
    Mu
  };
  K k;
  V c;
  E0<D> t;
  Symbol sym;
  T phi;
  E0<D> t1, t2;
  Exp0() : k(Term), c(D::zero()), phi{} {}

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
  static E0<D> mul(E0<D> lhs, E0<D> rhs) {
    auto e = std::make_shared<Exp0>();
    e->k = Mul;
    e->t1 = lhs;
    e->t2 = rhs;
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
  static E0<D> project(E0<D> t) {
    auto e = std::make_shared<Exp0>();
    e->k = Project;
    e->t = t;
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
  static E0<D> star(E0<D> body, Symbol x) {
    auto e = std::make_shared<Exp0>();
    e->k = Star;
    e->t = body;
    e->sym = x;
    return e;
  }
  static E0<D> mu(E0<D> body, Symbol x) {
    auto e = std::make_shared<Exp0>();
    e->k = Mu;
    e->t = body;
    e->sym = x;
    return e;
  }
};

template <class D> struct Exp1;
template <class D> using E1 = std::shared_ptr<Exp1<D>>;

template <class D>
bool referencesLocalSymbol(const E0<D> &expression, const Symbol &symbol,
                           std::unordered_set<const Exp0<D> *> &visited) {
  if (!expression || !visited.insert(expression.get()).second)
    return false;
  if (expression->k == Exp0<D>::Bound && expression->sym == symbol)
    return true;
  if (expression->k == Exp0<D>::Concat && expression->sym == symbol)
    return true;
  return referencesLocalSymbol(expression->t, symbol, visited) ||
         referencesLocalSymbol(expression->t1, symbol, visited) ||
         referencesLocalSymbol(expression->t2, symbol, visited);
}

template <class D>
bool referencesLocalSymbol(const E0<D> &expression, const Symbol &symbol) {
  std::unordered_set<const Exp0<D> *> visited;
  return referencesLocalSymbol(expression, symbol, visited);
}

/// Recognize the canonical equation Z = 1 + Z*a (or 1 + a*Z) used to encode
/// a semiring Kleene star. Domains with a closed-form star can evaluate this
/// shape without generic local fixpoint iteration.
template <class D> E0<D> matchSemiringStarOperand(const E0<D> &expression) {
  if (!expression || expression->k != Exp0<D>::Star || !expression->t ||
      expression->t->k != Exp0<D>::Ndet) {
    return nullptr;
  }

  auto isOne = [](const E0<D> &candidate) {
    return candidate && candidate->k == Exp0<D>::Term &&
           D::equal(candidate->c, D::one());
  };
  auto matchProduct = [&](const E0<D> &candidate) -> E0<D> {
    if (!candidate || candidate->k != Exp0<D>::Mul)
      return nullptr;
    if (candidate->t1 && candidate->t1->k == Exp0<D>::Bound &&
        candidate->t1->sym == expression->sym) {
      return candidate->t2;
    }
    if (candidate->t2 && candidate->t2->k == Exp0<D>::Bound &&
        candidate->t2->sym == expression->sym) {
      return candidate->t1;
    }
    return nullptr;
  };

  E0<D> operand;
  if (isOne(expression->t->t1))
    operand = matchProduct(expression->t->t2);
  else if (isOne(expression->t->t2))
    operand = matchProduct(expression->t->t1);
  if (referencesLocalSymbol(operand, expression->sym))
    return nullptr;
  return operand;
}

/// Linearized expression (right-hand side of Df|ν(X) + δ = X). Adds Add/Sub
/// for combine and differential; Concat/Star/Mu preserved from Exp0.
template <class D> struct Exp1 {
  using V = DomVal<D>;
  using T = DomTest<D>;
  enum K {
    Term,
    Seq,
    SeqR,
    Call,
    Cond,
    Ndet,
    Project,
    Hole,
    Bound,
    Concat,
    Star,
    Mu,
    Add,
    Sub
  };
  K k;
  V c;
  Symbol sym;
  T phi;
  E1<D> t, t1, t2;
  Exp1() : k(Term), c(D::zero()), phi{} {}

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
  static E1<D> project(E1<D> t) {
    auto e = std::make_shared<Exp1>();
    e->k = Project;
    e->t = t;
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
  static E1<D> star(E1<D> body, Symbol x) {
    auto e = std::make_shared<Exp1>();
    e->k = Star;
    e->t = body;
    e->sym = x;
    return e;
  }
  static E1<D> mu(E1<D> body, Symbol x) {
    auto e = std::make_shared<Exp1>();
    e->k = Mu;
    e->t = body;
    e->sym = x;
    return e;
  }
};

template <class D> struct ExprFeatureDetector {
  static bool has_star(const E0<D> &e) {
    return contains(e, Exp0<D>::Star);
  }

  static bool has_star(const E1<D> &e) {
    return contains(e, Exp1<D>::Star);
  }

  static bool has_mu(const E0<D> &e) {
    return contains(e, Exp0<D>::Mu);
  }

  static bool has_mu(const E1<D> &e) {
    return contains(e, Exp1<D>::Mu);
  }

  static bool has_project(const E0<D> &e) {
    return contains(e, Exp0<D>::Project);
  }

  static bool has_project(const E1<D> &e) {
    return contains(e, Exp1<D>::Project);
  }

private:
  static bool contains(const E0<D> &e, typename Exp0<D>::K kind) {
    std::unordered_set<const Exp0<D> *> visited;
    return contains(e, kind, visited);
  }

  static bool contains(const E0<D> &e, typename Exp0<D>::K kind,
                       std::unordered_set<const Exp0<D> *> &visited) {
    if (!e)
      return false;
    if (!visited.insert(e.get()).second)
      return false;
    if (e->k == kind)
      return true;
    return contains(e->t, kind, visited) ||
           contains(e->t1, kind, visited) ||
           contains(e->t2, kind, visited);
  }

  static bool contains(const E1<D> &e, typename Exp1<D>::K kind) {
    std::unordered_set<const Exp1<D> *> visited;
    return contains(e, kind, visited);
  }

  static bool contains(const E1<D> &e, typename Exp1<D>::K kind,
                       std::unordered_set<const Exp1<D> *> &visited) {
    if (!e)
      return false;
    if (!visited.insert(e.get()).second)
      return false;
    if (e->k == kind)
      return true;
    return contains(e->t, kind, visited) ||
           contains(e->t1, kind, visited) ||
           contains(e->t2, kind, visited);
  }
};

} // namespace npa

