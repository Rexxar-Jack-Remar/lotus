#ifndef NPA_EVAL_H
#define NPA_EVAL_H

#include "Dataflow/NPA/Core/Expressions.h"
#include "Dataflow/NPA/Core/Fixpoint.h"

namespace npa {

template <class D>
struct I0 {
  using V = DomVal<D>;
  using Map = std::unordered_map<Symbol, V>;
  static V eval(bool /*verbose*/, const Map &nu, const E0<D> &e) {
    mark(e);
    return rec(nu, {}, e);
  }

private:
  using Env = std::unordered_map<Symbol, V>;
  static void mark(const E0<D> &e) {
    if (!e) return;
    e->mark();
    switch (e->k) {
    case Exp0<D>::Seq:
      mark(e->t);
      break;
    case Exp0<D>::Cond:
      mark(e->t1);
      mark(e->t2);
      break;
    case Exp0<D>::Ndet:
      mark(e->t1);
      mark(e->t2);
      break;
    case Exp0<D>::Bound:
    case Exp0<D>::Hole:
      break;
    case Exp0<D>::Concat:
      mark(e->t1);
      mark(e->t2);
      break;
    case Exp0<D>::InfClos:
      mark(e->t);
      break;
    default:
      break;
    }
  }
  static V rec(const Map &nu, const Env &env, const E0<D> &e) {
    if (!e->dirty_) return *e->val;
    V v{};
    switch (e->k) {
    case Exp0<D>::Term:
      v = e->c;
      break;
    case Exp0<D>::Seq:
      v = D::extend(e->c, rec(nu, env, e->t));
      break;
    case Exp0<D>::Call:
      v = D::extend(nu.at(e->sym), rec(nu, env, e->t));
      break;
    case Exp0<D>::Cond:
      v = D::condCombine(e->phi, rec(nu, env, e->t1), rec(nu, env, e->t2));
      break;
    case Exp0<D>::Ndet:
      v = D::ndetCombine(rec(nu, env, e->t1), rec(nu, env, e->t2));
      break;
    case Exp0<D>::Hole:
      v = nu.at(e->sym);
      break;
    case Exp0<D>::Bound:
      v = env.at(e->sym);
      break;
    case Exp0<D>::Concat: {
      // Linear context-free form: t1 · X · t2
      auto it = env.find(e->sym);
      const V &mid = (it != env.end()) ? it->second : nu.at(e->sym);
      v = D::extend(rec(nu, env, e->t1), D::extend(mid, rec(nu, env, e->t2)));
    } break;
    case Exp0<D>::InfClos: {
      V init = D::zero();
      v = fix<D>(false, init, [&](V cur) {
        auto env2 = env;
        env2[e->sym] = cur;
        mark(e->t);
        return rec(nu, env2, e->t);
      });
    } break;
    }
    e->val = v;
    e->mark(false);
    return v;
  }
};

template <class D>
struct I1 {
  using V = DomVal<D>;
  using Map = std::unordered_map<Symbol, V>;
  static V eval(bool /*verbose*/, const Map &vars, const E1<D> &e) {
    mark(e);
    return rec(vars, {}, e);
  }

private:
  using Env = std::unordered_map<Symbol, V>;
  static void mark(const E1<D> &e) {
    if (!e) return;
    e->mark();
    if (e->t) mark(e->t);
    if (e->t1) mark(e->t1);
    if (e->t2) mark(e->t2);
  }
  static V rec(const Map &vars, const Env &env, const E1<D> &e) {
    if (!e->dirty_) return *e->val;
    V v{};
    using K = typename Exp1<D>::K;
    switch (e->k) {
    case K::Term:
      v = e->c;
      break;
    case K::Seq:
      v = D::extend(e->c, rec(vars, env, e->t));
      break;
    case K::Call: {
      auto it = env.find(e->sym);
      const V &f = (it != env.end()) ? it->second : vars.at(e->sym);
      v = D::extend(f, e->c);
    } break;
    case K::Cond:
      v = D::condCombine(e->phi, rec(vars, env, e->t1), rec(vars, env, e->t2));
      break;
    case K::Add:
      v = D::combine(rec(vars, env, e->t1), rec(vars, env, e->t2));
      break;
    case K::Sub:
      v = D::subtract(rec(vars, env, e->t1), rec(vars, env, e->t2));
      break;
    case K::Ndet:
      v = D::ndetCombine(rec(vars, env, e->t1), rec(vars, env, e->t2));
      break;
    case K::Hole:
      v = vars.at(e->sym);
      break;
    case K::Bound:
      v = env.at(e->sym);
      break;
    case K::Concat: {
      auto it = env.find(e->sym);
      const V &mid = (it != env.end()) ? it->second : vars.at(e->sym);
      v = D::extend(rec(vars, env, e->t1), D::extend(mid, rec(vars, env, e->t2)));
    } break;
    case K::InfClos: {
      V init = D::zero();
      v = fix<D>(false, init, [&](V cur) {
        auto env2 = env;
        env2[e->sym] = cur;
        mark(e->t);
        return rec(vars, env2, e->t);
      });
    } break;
    }
    e->val = v;
    e->mark(false);
    return v;
  }
};

} // namespace npa

#endif // NPA_EVAL_H
