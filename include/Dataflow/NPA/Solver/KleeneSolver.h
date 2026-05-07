#ifndef NPA_KLEENE_SOLVER_H
#define NPA_KLEENE_SOLVER_H

/**
 * \file
 * \brief Public Kleene solver for whole equation systems `X = f(X)`.
 *
 * `KleeneSolver<D>` applies direct Kleene iteration to the full system.
 * Unlike `NPASolver<D>`, it does not build or solve a Newton linearized
 * correction system, and it does not use `LinearStrategy`.
 */

#include "Dataflow/NPA/Core/Expr/Eval.h"
#include "Dataflow/NPA/Solver/EquationSystem.h"

#include <unordered_map>

namespace npa {

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
      LinearStrategy = LinearStrategy::SCC) {
    std::unordered_map<Symbol, V> nu;
    for (auto &b : binds)
      nu[b.first] = b.second;
    std::vector<std::pair<Symbol, V>> out;
    for (auto &e : eqns)
      out.emplace_back(e.first, I0<D>::eval(verbose, nu, e.second));
    return out;
  }
};

template <class D> struct KleeneSolver {
  using V = DomVal<D>;
  using Eqn = std::pair<Symbol, E0<D>>;
  static std::pair<std::vector<std::pair<Symbol, V>>, Stat>
  solve(const std::vector<Eqn> &eqns, bool verbose = false, int max = -1,
        DomainContractMode contractMode = DomainContractMode::Off) {
    return EquationSystemSolver<D, KleeneIter<D>>::solve(
        eqns, verbose, max, LinearStrategy::SCC, contractMode);
  }
};

} // namespace npa

#endif // NPA_KLEENE_SOLVER_H
