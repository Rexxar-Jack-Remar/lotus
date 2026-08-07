#ifndef NPA_TENSOR_DIFFERENTIAL_H
#define NPA_TENSOR_DIFFERENTIAL_H

#include "Dataflow/NPA/Core/Expr/Eval.h"
#include "Dataflow/NPA/Solver/Newton/Errors.h"
#include "Dataflow/NPA/Solver/Newton/Linear/Tensor/TensorSemiring.h"

#include <mutex>
#include <sstream>

namespace npa {

/// Direct tensor-side differential builder for the optional tensor backend.
template <class D> struct TensorDiff {
  using V = DomVal<D>;
  using Traits = TensorSemiringTraits<D>;
  using TD = typename Traits::tensor_domain;
  using Map = std::unordered_map<Symbol, V>;
  using Env = typename I0<D>::Environment;
  using EvalContext = typename I0<D>::EvaluationContext;
  struct Plan {
    std::vector<typename Exp0<D>::K> preorder;
    bool has_mu = false;
    bool has_project = false;
  };

  static E1<TD> build(const Map &nu, const E0<D> &e) {
    validate(e);
    EvalContext context;
    (void)I0<D>::evalWithContext(nu, {}, e, context);
    return aux(nu, {}, context, e);
  }

  static E1<TD> build(const Map &nu, const E0<D> &e,
                      const EvalContext &context) {
    validate(e);
    return aux(nu, {}, context, e);
  }

private:
  static void validate(const E0<D> &e) {
    auto plan = get_plan(e);
    if ((*plan).has_mu)
      throw UnsupportedNewtonMuError{};
    if ((*plan).has_project && !domain_project_newton_safe<D>())
      throw UnsafeNewtonProjectError{};
  }

  static void describe(const E0<D> &e, Plan &plan,
                       std::ostringstream &signature) {
    if (!e) {
      signature << "N;";
      return;
    }
    plan.preorder.push_back(e->k);
    signature << static_cast<int>(e->k) << ';';
    using K0 = typename Exp0<D>::K;
    switch (e->k) {
    case K0::Project:
      plan.has_project = true;
      describe(e->t, plan, signature);
      break;
    case K0::Mu:
      plan.has_mu = true;
      describe(e->t, plan, signature);
      break;
    case K0::Seq:
    case K0::Call:
    case K0::Star:
      describe(e->t, plan, signature);
      break;
    case K0::Mul:
    case K0::Cond:
    case K0::Ndet:
    case K0::Concat:
      describe(e->t1, plan, signature);
      describe(e->t2, plan, signature);
      break;
    default:
      break;
    }
  }

  static Optional<Plan> get_plan(const E0<D> &e) {
    static std::mutex cache_mu;
    static std::unordered_map<std::string, Plan> cache;

    Plan plan;
    std::ostringstream signature;
    describe(e, plan, signature);
    const std::string key = signature.str();
    std::lock_guard<std::mutex> lock(cache_mu);
    auto inserted = cache.emplace(key, plan);
    return inserted.first->second;
  }

  static E1<TD> aux(const Map &nu, const Env &env,
                    const EvalContext &context, const E0<D> &expr) {
    using K0 = typename Exp0<D>::K;
    switch (expr->k) {
    case K0::Term:
      return Exp1<TD>::term(TD::zero());
    case K0::Seq:
      return Exp1<TD>::seqR(aux(nu, env, context, expr->t),
                            Traits::left_constant(expr->c));
    case K0::Mul: {
      auto lhs = aux(nu, env, context, expr->t1);
      auto rhs = aux(nu, env, context, expr->t2);
      return Exp1<TD>::add(
          Exp1<TD>::seqR(lhs,
                         Traits::right_constant(context.valueOf(expr->t2))),
          Exp1<TD>::seqR(rhs,
                         Traits::left_constant(context.valueOf(expr->t1))));
    }
    case K0::Call: {
      auto d_arg = aux(nu, env, context, expr->t);
      auto left =
          Exp1<TD>::seqR(d_arg, Traits::left_constant(nu.at(expr->sym)));
      auto right = Exp1<TD>::seqR(
          Exp1<TD>::hole(expr->sym),
          Traits::right_constant(context.valueOf(expr->t)));
      return Exp1<TD>::add(left, right);
    }
    case K0::Cond:
      return Exp1<TD>::cond(expr->phi, aux(nu, env, context, expr->t1),
                            aux(nu, env, context, expr->t2));
    case K0::Ndet:
      return Exp1<TD>::add(aux(nu, env, context, expr->t1),
                           aux(nu, env, context, expr->t2));
    case K0::Project:
      return Exp1<TD>::project(aux(nu, env, context, expr->t));
    case K0::Hole:
      return Exp1<TD>::hole(expr->sym);
    case K0::Bound:
      return Exp1<TD>::term(TD::zero());
    case K0::Concat: {
      V lhs_value = context.valueOf(expr->t1);
      V rhs_value = context.valueOf(expr->t2);
      auto bound = env.find(expr->sym);
      V middle = bound != env.end() ? bound->second : nu.at(expr->sym);
      auto lhs_diff = aux(nu, env, context, expr->t1);
      auto rhs_diff = aux(nu, env, context, expr->t2);
      auto term1 = Exp1<TD>::seqR(
          lhs_diff, Traits::right_constant(D::extend(middle, rhs_value)));
      auto term2 = Exp1<TD>::seqR(Exp1<TD>::hole(expr->sym),
                                  Traits::couple(lhs_value, rhs_value));
      auto term3 = Exp1<TD>::seqR(
          rhs_diff, Traits::left_constant(D::extend(lhs_value, middle)));
      return Exp1<TD>::add(Exp1<TD>::add(term1, term2), term3);
    }
    case K0::Star: {
      V star_value = context.valueOf(expr);
      Env body_env = env;
      body_env[expr->sym] = star_value;
      return Exp1<TD>::seqR(aux(nu, body_env, context, expr->t),
                            Traits::couple(star_value, star_value));
    }
    case K0::Mu:
      throw UnsupportedNewtonMuError{};
    }
    return nullptr;
  }
};

} // namespace npa

#endif // NPA_TENSOR_DIFFERENTIAL_H
