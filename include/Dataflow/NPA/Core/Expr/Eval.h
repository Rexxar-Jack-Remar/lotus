#ifndef NPA_EVAL_H
#define NPA_EVAL_H

/**
 * \file
 * \brief Evaluators for polynomial (I0) and linearized (I1) expressions.
 *
 * - I0: evaluates Exp0 (full system f(X)) under a valuation ν for variables.
 *   Used for Kleene step κ^(i+1) = f(κ^(i)) and for Newton step f(ν^(i)).
 * - I1: evaluates Exp1 (linearized RHS) under a valuation for variables.
 *   Used to compute the least solution of Df|ν(X) + δ = X by iterative
 *   substitution (worklist, SCC, or after tensor conversion).
 *
 * Concat is evaluated as extend(t1_val, extend(mid, t2_val)) (LCFL form
 * t1·X·t2). Star and Mu are least fixpoints in the bound variable.
 */

#include "Dataflow/NPA/Core/Expr/Expressions.h"
#include "Dataflow/NPA/Solver/Fixpoint.h"

namespace npa {

/// Evaluator for polynomial expressions (Exp0).
template <class D> struct I0 {
  using V = DomVal<D>;
  using Map = std::unordered_map<Symbol, V>;
  using Environment = std::unordered_map<Symbol, V>;

  struct EvaluationContext {
    std::unordered_map<const Exp0<D> *, V> values;

    void invalidate(const E0<D> &expr) {
      if (!expr)
        return;
      values.erase(expr.get());
      if (expr->t)
        invalidate(expr->t);
      if (expr->t1)
        invalidate(expr->t1);
      if (expr->t2)
        invalidate(expr->t2);
    }

    const V &valueOf(const E0<D> &expr) const { return values.at(expr.get()); }
  };

  static V eval(bool /*verbose*/, const Map &nu, const E0<D> &e) {
    EvaluationContext context;
    return rec(nu, {}, e, context);
  }

  static V evalWithEnvironment(const Map &nu, const Environment &env,
                               const E0<D> &e) {
    EvaluationContext context;
    return rec(nu, env, e, context);
  }

  static V evalWithContext(const Map &nu, const Environment &env,
                           const E0<D> &e, EvaluationContext &context) {
    context.invalidate(e);
    return rec(nu, env, e, context);
  }

private:
  static V rec(const Map &nu, const Environment &env, const E0<D> &e,
               EvaluationContext &context) {
    auto cached = context.values.find(e.get());
    if (cached != context.values.end())
      return cached->second;

    V v{};
    switch (e->k) {
    case Exp0<D>::Term:
      v = e->c;
      break;
    case Exp0<D>::Seq:
      v = D::extend(e->c, rec(nu, env, e->t, context));
      break;
    case Exp0<D>::Mul:
      v = D::extend(rec(nu, env, e->t1, context),
                    rec(nu, env, e->t2, context));
      break;
    case Exp0<D>::Call:
      v = D::extend(nu.at(e->sym), rec(nu, env, e->t, context));
      break;
    case Exp0<D>::Cond:
      v = D::condCombine(e->phi, rec(nu, env, e->t1, context),
                         rec(nu, env, e->t2, context));
      break;
    case Exp0<D>::Ndet:
      v = D::ndetCombine(rec(nu, env, e->t1, context),
                         rec(nu, env, e->t2, context));
      break;
    case Exp0<D>::Project:
      v = domain_project<D>(rec(nu, env, e->t, context));
      break;
    case Exp0<D>::Hole:
      v = nu.at(e->sym);
      break;
    case Exp0<D>::Bound:
      v = env.at(e->sym);
      break;
    case Exp0<D>::Concat: {
      // LCFL form t1·X·t2: value = t1_val ⊗ mid ⊗ t2_val (Reps et al. §3.1).
      auto it = env.find(e->sym);
      const V &mid = (it != env.end()) ? it->second : nu.at(e->sym);
      v = D::extend(rec(nu, env, e->t1, context),
                    D::extend(mid, rec(nu, env, e->t2, context)));
    } break;
    case Exp0<D>::Star:
    case Exp0<D>::Mu: {
      V init = D::zero();
      v = fix<D>(false, init, [&](V cur) {
        auto env2 = env;
        env2[e->sym] = cur;
        context.invalidate(e->t);
        return rec(nu, env2, e->t, context);
      });
    } break;
    }
    context.values[e.get()] = v;
    return v;
  }
};

/// Evaluator for linearized expressions (Exp1). Used when solving the
/// linear system Df|ν(X) + δ = X (worklist, SCC, or tensor space).
template <class D> struct I1 {
  using V = DomVal<D>;
  using Map = std::unordered_map<Symbol, V>;
  using Env = std::unordered_map<Symbol, V>;

  struct EvaluationContext {
    std::unordered_map<const Exp1<D> *, V> values;

    void invalidate(const E1<D> &expr) {
      if (!expr)
        return;
      values.erase(expr.get());
      if (expr->t)
        invalidate(expr->t);
      if (expr->t1)
        invalidate(expr->t1);
      if (expr->t2)
        invalidate(expr->t2);
    }
  };

  static V eval(bool /*verbose*/, const Map &vars, const E1<D> &e) {
    return evalWithLookup(false,
                          [&](const Symbol &sym) -> const V & {
                            return vars.at(sym);
                          },
                          e);
  }

  template <class Lookup>
  static V evalWithLookup(bool /*verbose*/, const Lookup &lookup,
                          const E1<D> &e) {
    EvaluationContext context;
    return rec(lookup, {}, e, context);
  }

private:
  template <class Lookup>
  static V rec(const Lookup &lookup, const Env &env, const E1<D> &e,
               EvaluationContext &context) {
    auto cached = context.values.find(e.get());
    if (cached != context.values.end())
      return cached->second;

    V v{};
    using K = typename Exp1<D>::K;
    switch (e->k) {
    case K::Term:
      v = e->c;
      break;
    case K::Seq:
      v = D::extend_lin(e->c, rec(lookup, env, e->t, context));
      break;
    case K::SeqR:
      v = D::extend_lin(rec(lookup, env, e->t, context), e->c);
      break;
    case K::Call: {
      auto it = env.find(e->sym);
      const V &f = (it != env.end()) ? it->second : lookup(e->sym);
      v = D::extend_lin(f, e->c);
    } break;
    case K::Cond:
      v = D::condCombine(e->phi, rec(lookup, env, e->t1, context),
                         rec(lookup, env, e->t2, context));
      break;
    case K::Add:
      v = D::combine(rec(lookup, env, e->t1, context),
                     rec(lookup, env, e->t2, context));
      break;
    case K::Sub:
      v = D::subtract(rec(lookup, env, e->t1, context),
                      rec(lookup, env, e->t2, context));
      break;
    case K::Ndet:
      v = D::ndetCombine(rec(lookup, env, e->t1, context),
                         rec(lookup, env, e->t2, context));
      break;
    case K::Project:
      v = domain_project<D>(rec(lookup, env, e->t, context));
      break;
    case K::Hole:
      v = lookup(e->sym);
      break;
    case K::Bound:
      v = env.at(e->sym);
      break;
    case K::Concat: {
      // LCFL: a·Y·b -> a_val ⊗ Y ⊗ b_val (coefficients on both sides).
      auto it = env.find(e->sym);
      const V &mid = (it != env.end()) ? it->second : lookup(e->sym);
      v = D::extend_lin(rec(lookup, env, e->t1, context),
                        D::extend_lin(mid,
                                      rec(lookup, env, e->t2, context)));
    } break;
    case K::Star:
    case K::Mu: {
      V init = D::zero();
      v = fix<D>(false, init, [&](V cur) {
        auto env2 = env;
        env2[e->sym] = cur;
        context.invalidate(e->t);
        return rec(lookup, env2, e->t, context);
      });
    } break;
    }
    context.values[e.get()] = v;
    return v;
  }
};

} // namespace npa

#endif // NPA_EVAL_H
