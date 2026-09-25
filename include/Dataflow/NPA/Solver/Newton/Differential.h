#pragma once

/**
 * \file
 * \brief Differential construction: builds linearized expression Df|nu.
 *
 * Given the current approximation nu and a polynomial expression e, build the
 * Exp1 differential used by a Newton round. Input expressions may be DAGs;
 * both validation and differentiation preserve shared subexpressions.
 */

#include "Dataflow/NPA/Core/Expr/Eval.h"
#include "Dataflow/NPA/Solver/Newton/Errors.h"

#include <cstddef>
#include <unordered_map>
#include <unordered_set>

namespace npa {

template <class D> struct Diff {
  using V = DomVal<D>;
  using M0 = E0<D>;
  using M1 = E1<D>;
  using Map = std::unordered_map<Symbol, V>;
  using Env = typename I0<D>::Environment;
  using EvalContext = typename I0<D>::EvaluationContext;

  struct Plan {
    bool has_mu = false;
    bool has_project = false;
  };

  /// Build the differential of e at nu without mutating the expression DAG.
  static M1 build(const Map &nu, const M0 &e) {
    requireSupported(e);
    EvalContext evaluationContext;
    (void)I0<D>::evalWithContext(nu, {}, e, evaluationContext);
    BuildContext buildContext;
    return aux(nu, {}, evaluationContext, e, buildContext, 0);
  }

  static M1 build(const Map &nu, const M0 &e,
                  const EvalContext &evaluationContext) {
    requireSupported(e);
    BuildContext buildContext;
    return aux(nu, {}, evaluationContext, e, buildContext, 0);
  }

private:
  struct MemoKey {
    const Exp0<D> *expression = nullptr;
    std::size_t scope = 0;

    bool operator==(const MemoKey &other) const {
      return expression == other.expression && scope == other.scope;
    }
  };

  struct MemoKeyHash {
    std::size_t operator()(const MemoKey &key) const {
      const auto pointerHash = std::hash<const Exp0<D> *>{}(key.expression);
      return pointerHash ^ (key.scope + static_cast<std::size_t>(0x9e3779b9) +
                            (pointerHash << 6) + (pointerHash >> 2));
    }
  };

  struct BuildContext {
    std::unordered_map<MemoKey, M1, MemoKeyHash> memo;
    std::size_t next_scope = 1;
  };

  static void inspect(const M0 &expression, Plan &plan,
                      std::unordered_set<const Exp0<D> *> &visited) {
    if (!expression || !visited.insert(expression.get()).second)
      return;

    using K = typename Exp0<D>::K;
    switch (expression->k) {
    case K::Project:
      plan.has_project = true;
      inspect(expression->t, plan, visited);
      return;
    case K::Mu:
      plan.has_mu = true;
      inspect(expression->t, plan, visited);
      return;
    case K::Seq:
    case K::Call:
    case K::Star:
      inspect(expression->t, plan, visited);
      return;
    case K::Mul:
    case K::Cond:
    case K::Ndet:
    case K::Concat:
      inspect(expression->t1, plan, visited);
      inspect(expression->t2, plan, visited);
      return;
    case K::Term:
    case K::Hole:
    case K::Bound:
      return;
    }
  }

  static Plan makePlan(const M0 &expression) {
    Plan plan;
    std::unordered_set<const Exp0<D> *> visited;
    inspect(expression, plan, visited);
    return plan;
  }

  static void requireSupported(const M0 &expression) {
    const Plan plan = makePlan(expression);
    if (plan.has_mu)
      throw UnsupportedNewtonMuError{};
    if (plan.has_project && !domain_project_newton_safe<D>())
      throw UnsafeNewtonProjectError{};
  }

  static M1 aux(const Map &nu, const Env &env,
                const EvalContext &evaluationContext, const M0 &expression,
                BuildContext &buildContext, std::size_t scope) {
    const MemoKey key{expression.get(), scope};
    auto cached = buildContext.memo.find(key);
    if (cached != buildContext.memo.end())
      return cached->second;

    M1 result = auxUncached(nu, env, evaluationContext, expression,
                            buildContext, scope);
    buildContext.memo.emplace(key, result);
    return result;
  }

  static M1 auxUncached(const Map &nu, const Env &env,
                        const EvalContext &evaluationContext,
                        const M0 &expression, BuildContext &buildContext,
                        std::size_t scope) {
    using K = typename Exp0<D>::K;
    switch (expression->k) {
    case K::Term:
      return Exp1<D>::term(D::zero());
    case K::Seq:
      return Exp1<D>::seq(
          expression->c,
          aux(nu, env, evaluationContext, expression->t, buildContext, scope));
    case K::Mul: {
      M1 lhs = aux(nu, env, evaluationContext, expression->t1, buildContext,
                   scope);
      M1 rhs = aux(nu, env, evaluationContext, expression->t2, buildContext,
                   scope);
      return Exp1<D>::add(
          Exp1<D>::seqR(lhs, evaluationContext.valueOf(expression->t2)),
          Exp1<D>::seq(evaluationContext.valueOf(expression->t1), rhs));
    }
    case K::Call: {
      M1 argument = aux(nu, env, evaluationContext, expression->t,
                        buildContext, scope);
      M1 argumentTerm = Exp1<D>::seq(nu.at(expression->sym), argument);
      M1 calleeTerm = Exp1<D>::call(
          expression->sym, evaluationContext.valueOf(expression->t));
      return Exp1<D>::add(argumentTerm, calleeTerm);
    }
    case K::Cond:
      return Exp1<D>::cond(
          expression->phi,
          aux(nu, env, evaluationContext, expression->t1, buildContext, scope),
          aux(nu, env, evaluationContext, expression->t2, buildContext,
              scope));
    case K::Ndet:
      return Exp1<D>::add(
          aux(nu, env, evaluationContext, expression->t1, buildContext, scope),
          aux(nu, env, evaluationContext, expression->t2, buildContext,
              scope));
    case K::Project:
      return Exp1<D>::project(aux(nu, env, evaluationContext, expression->t,
                                  buildContext, scope));
    case K::Hole:
      return Exp1<D>::hole(expression->sym);
    case K::Bound:
      // Bound variables are constants with respect to system variables.
      return Exp1<D>::term(D::zero());
    case K::Concat: {
      const V &leftValue = evaluationContext.valueOf(expression->t1);
      const V &rightValue = evaluationContext.valueOf(expression->t2);
      auto bound = env.find(expression->sym);
      const V &middleValue =
          bound != env.end() ? bound->second : nu.at(expression->sym);
      M1 leftDerivative = aux(nu, env, evaluationContext, expression->t1,
                              buildContext, scope);
      M1 rightDerivative = aux(nu, env, evaluationContext, expression->t2,
                               buildContext, scope);
      M1 leftTerm = Exp1<D>::seqR(
          leftDerivative, D::extend(middleValue, rightValue));
      M1 middleTerm = Exp1<D>::concat(Exp1<D>::term(leftValue),
                                      expression->sym,
                                      Exp1<D>::term(rightValue));
      M1 rightTerm = Exp1<D>::seq(
          leftValue, Exp1<D>::seq(middleValue, rightDerivative));
      return Exp1<D>::add(Exp1<D>::add(leftTerm, middleTerm), rightTerm);
    }
    case K::Star: {
      // TOPLAS 2016, Eq. (60): D(g*) = g(nu)* D(g) g(nu)*.
      const V &starValue = evaluationContext.valueOf(expression);
      if constexpr (DomainHasStar<D>::value) {
        if (M0 operand = matchSemiringStarOperand<D>(expression)) {
          M1 operandDerivative = aux(nu, env, evaluationContext, operand,
                                     buildContext, scope);
          return Exp1<D>::seq(
              starValue,
              Exp1<D>::seqR(std::move(operandDerivative), starValue));
        }
      }
      Env bodyEnvironment = env;
      bodyEnvironment.insert_or_assign(expression->sym, starValue);
      const std::size_t bodyScope = buildContext.next_scope++;
      M1 bodyDerivative =
          aux(nu, bodyEnvironment, evaluationContext, expression->t,
              buildContext, bodyScope);
      return Exp1<D>::seq(
          starValue, Exp1<D>::seqR(std::move(bodyDerivative), starValue));
    }
    case K::Mu:
      throw UnsupportedNewtonMuError{};
    }
    return nullptr;
  }
};

} // namespace npa

