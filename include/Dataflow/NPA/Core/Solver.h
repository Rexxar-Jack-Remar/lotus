#ifndef NPA_SOLVER_H
#define NPA_SOLVER_H

/**
 * \file
 * \brief Outer iteration: Kleene vs Newton; dispatches linear strategy.
 *
 * Solver<D, ITER> runs ITER until the vector of equation values stabilizes.
 * - KleeneIter: κ^(i+1) = f(κ^(i)) (classical Kleene sequence, Eqn. (1) in
 *   Esparza et al.).
 * - NewtonIter: ν^(0) = f(⊥); ν^(i+1) = ν^(i) ⊔ LinearCorrectionTerm.
 *   The correction is Δ^(i) = least solution of Df|ν^(i)(X) + δ^(i) = X
 *   (Eqn. (2), (13)); then ν^(i+1) = ν^(i) ⊕ Δ^(i) (idempotent) or
 *   ν^(i+1) = ν^(i) + Δ^(i) (non-idempotent). The linear system is solved
 *   by Naive, Worklist, SCC, or TensorProduct (LinearStrategy).
 *
 * References: Esparza et al. (JACM); Reps et al. (TOPLAS 2016).
 */

#include "Dataflow/NPA/Core/TensorLinearSolve.h"

namespace npa {

namespace detail {
/// C++14-friendly dispatch for delta: avoid if constexpr (DomainHasChooseDelta,
/// idempotent) → choose_delta(v, nu) or v; else subtract(v, nu) or v.
template <class D>
DomVal<D> compute_delta(const DomVal<D> &v, const DomVal<D> &nu_sym,
                        std::true_type /* has_choose_delta */,
                        std::true_type /* idempotent */) {
  return v;
}
template <class D>
DomVal<D> compute_delta(const DomVal<D> &v, const DomVal<D> &nu_sym,
                        std::true_type /* has_choose_delta */,
                        std::false_type /* idempotent */) {
  return D::choose_delta(v, nu_sym);
}
template <class D>
DomVal<D> compute_delta(const DomVal<D> &v, const DomVal<D> &nu_sym,
                        std::false_type /* has_choose_delta */,
                        std::true_type /* idempotent */) {
  return v;
}
template <class D>
DomVal<D> compute_delta(const DomVal<D> &v, const DomVal<D> &nu_sym,
                        std::false_type /* has_choose_delta */,
                        std::false_type /* idempotent */) {
  return D::subtract(v, nu_sym);
}
} // namespace detail

template <class D, class ITER> struct Solver {
  using V = DomVal<D>;
  using Eqn = std::pair<Symbol, E0<D>>;
  static std::pair<std::vector<std::pair<Symbol, V>>, Stat>
  solve(const std::vector<Eqn> &eqns, bool verbose = false, int max = -1,
        LinearStrategy linStrat = LinearStrategy::Worklist) {
    NPA_REQUIRE_DOMAIN(D);
    std::vector<std::pair<Symbol, V>> cur = ITER::init(eqns);
    auto tic = std::chrono::high_resolution_clock::now();
    int it = 0;
    while (max < 0 || it < max) {
      auto nxt = ITER::run(verbose, eqns, cur, linStrat);
      bool stable = true;
      for (size_t i = 0; i < cur.size(); ++i)
        if (!domain_equal<D>(cur[i].second, nxt[i].second)) {
          stable = false;
          break;
        }
      cur.swap(nxt);
      ++it;
      if (stable) {
        if (verbose)
          std::cerr << "[conv] " << it << "\n";
        break;
      }
    }
    auto toc = std::chrono::high_resolution_clock::now();
    Stat st;
    st.iters = it;
    st.time = std::chrono::duration<double>(toc - tic).count();
    return {cur, st};
  }
};

/// Kleene iteration: one round = evaluate all equations under current ν.
/// κ^(i+1) = f(κ^(i)); no linear correction (Esparza et al. Eqn. (1)).
template <class D> struct KleeneIter {
  using V = DomVal<D>;
  using Eqn = std::pair<Symbol, E0<D>>;
  static std::vector<std::pair<Symbol, V>> init(const std::vector<Eqn> &eqns) {
    std::vector<std::pair<Symbol, V>> cur;
    cur.reserve(eqns.size());
    for (auto &e : eqns)
      cur.emplace_back(e.first, D::zero());
    return cur;
  }
  static std::vector<std::pair<Symbol, V>>
  run(bool verbose, const std::vector<Eqn> &eqns,
      const std::vector<std::pair<Symbol, V>> &binds,
      LinearStrategy = LinearStrategy::Worklist) {
    std::unordered_map<Symbol, V> nu;
    for (auto &b : binds)
      nu[b.first] = b.second;
    std::vector<std::pair<Symbol, V>> out;
    for (auto &e : eqns)
      out.emplace_back(e.first, I0<D>::eval(verbose, nu, e.second));
    return out;
  }
};

/// Newton iteration: one round = f(ν) plus least solution of Df|ν(X)+δ = X.
/// δ = f(ν)−ν (or f(ν) when idempotent); Δ = solve linear system; ν' = ν⊕Δ.
template <class D> struct NewtonIter {
  using V = DomVal<D>;
  using Eqn = std::pair<Symbol, E0<D>>;
  static std::vector<std::pair<Symbol, V>> init(const std::vector<Eqn> &eqns) {
    std::unordered_map<Symbol, V> nu0;
    for (auto &e : eqns)
      nu0[e.first] = D::zero();
    std::vector<std::pair<Symbol, V>> cur;
    cur.reserve(eqns.size());
    for (auto &e : eqns)
      cur.emplace_back(e.first, I0<D>::eval(false, nu0, e.second));
    return cur;
  }
  static std::vector<std::pair<Symbol, V>>
  run(bool verbose, const std::vector<Eqn> &eqns,
      const std::vector<std::pair<Symbol, V>> &binds,
      LinearStrategy linStrat = LinearStrategy::Worklist) {
    std::unordered_map<Symbol, V> nu;
    for (auto &b : binds)
      nu[b.first] = b.second;
    std::vector<std::pair<Symbol, E1<D>>> rhs;
    using TensorTraits = TensorSemiringTraits<D>;
    using TD = typename TensorTraits::tensor_domain;
    std::vector<std::pair<Symbol, E1<TD>>> rhs_tensor;
    const bool use_tensor =
        linStrat == LinearStrategy::TensorProduct && TensorTraits::available();
    if (linStrat == LinearStrategy::TensorProduct && !use_tensor && verbose)
      std::cerr << "[tensor] tensor traits unavailable for domain; "
                   "falling back to worklist\n";
    for (auto &e : eqns) {
      V v = I0<D>::eval(verbose, nu, e.second);
      V delta0 = detail::compute_delta<D>(
          v, nu[e.first],
          std::integral_constant<bool, DomainHasChooseDelta<D>::value>{},
          std::integral_constant<bool, D::idempotent>{});
      if (use_tensor) {
        rhs_tensor.emplace_back(
            e.first,
            Exp1<TD>::add(Exp1<TD>::term(TensorTraits::right_constant(delta0)),
                          TensorDiff<D>::build(nu, e.second)));
      } else {
        auto d = Diff<D>::build(nu, e.second);
        rhs.emplace_back(e.first, Exp1<D>::add(Exp1<D>::term(delta0), d));
      }
    }
    std::vector<V> init(use_tensor ? rhs_tensor.size() : rhs.size(),
                        D::zero()),
        delta;
    if (linStrat == LinearStrategy::Naive) {
      delta = fix_vec<D>(verbose, init, [&](const std::vector<V> &cur) {
        std::unordered_map<Symbol, V> env;
        for (size_t i = 0; i < cur.size(); ++i)
          env[rhs[i].first] = cur[i];
        std::vector<V> nxt;
        for (auto &p : rhs)
          nxt.push_back(I1<D>::eval(false, env, p.second));
        return nxt;
      });
    } else if (linStrat == LinearStrategy::SCC) {
      delta = solve_linear_scc_impl<D>(verbose, rhs, init);
    } else if (use_tensor) {
      delta = solve_linear_tensorized_impl<D>(verbose, rhs_tensor, init);
    } else {
      delta = solve_linear_worklist_impl<D>(verbose, rhs, init);
    }
    std::vector<std::pair<Symbol, V>> out;
    for (size_t i = 0; i < binds.size(); ++i) {
      V upd = delta[i];
      V nxt = D::idempotent ? upd : D::combine(binds[i].second, upd);
      out.emplace_back(binds[i].first, nxt);
    }
    return out;
  }
};

template <class D> using KleeneSolver = Solver<D, KleeneIter<D>>;
template <class D> struct NewtonSolver {
  using V = DomVal<D>;
  using Eqn = std::pair<Symbol, E0<D>>;
  static std::pair<std::vector<std::pair<Symbol, V>>, Stat>
  solve(const std::vector<Eqn> &eqns, bool verbose = false, int max = -1,
        LinearStrategy linStrat = LinearStrategy::Worklist) {
    // JACM (Esparza et al.) shows: for idempotent + commutative semirings,
    // Newton terminates after at most n iterations for a system of n equations.
    // We only apply this bound when the domain explicitly declares
    // commutativity.
    int effective_max = max;
    if (effective_max < 0 && D::idempotent && domain_commutative_extend<D>()) {
      effective_max = static_cast<int>(eqns.size());
    }
    return Solver<D, NewtonIter<D>>::solve(eqns, verbose, effective_max,
                                           linStrat);
  }
};

} // namespace npa

#endif // NPA_SOLVER_H
