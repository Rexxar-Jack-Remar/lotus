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
#include "Dataflow/NPA/Solver/DomainValidation.h"
#include "Dataflow/NPA/Solver/SolveContext.h"
#include "Dataflow/NPA/Solver/Statistics.h"

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
      const std::vector<std::pair<Symbol, V>> &binds) {
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
    NPA_REQUIRE_DOMAIN(D);
    SolveContext<D> context;
    context.options.verbose = verbose;
    context.options.max_iterations = max;
    context.options.contract_mode = contractMode;
    const bool checks_run = contractMode == DomainContractMode::BasicChecks;
    const bool contract_ok =
        !checks_run || run_basic_domain_contract_checks<D>(verbose);
    auto result = iterate_until_stable(
        KleeneIter<D>::init(eqns),
        [&](const std::vector<std::pair<Symbol, V>> &current) {
          return KleeneIter<D>::run(verbose, eqns, current);
        },
        [](const std::vector<std::pair<Symbol, V>> &lhs,
           const std::vector<std::pair<Symbol, V>> &rhs) {
          for (std::size_t i = 0; i < lhs.size(); ++i)
            if (!domain_equal<D>(lhs[i].second, rhs[i].second))
              return false;
          return true;
        },
        max, verbose);

    if (!result.stabilized && max >= 0) {
      npa_note_outer_limit_hit();
      if (verbose)
        std::cerr << "[conv] hit outer iteration cap=" << max << "\n";
    }

    Stat &stats = context.stats;
    stats.iters = result.iterations;
    stats.time = result.seconds;
    stats.hit_limit = npa_limit_hit();
    stats.hit_outer_limit = npa_hit_outer_limit();
    stats.hit_fixpoint_limit = npa_hit_fixpoint_limit();
    stats.equation_count = static_cast<int>(eqns.size());
    stats.requested_max_iters = max;
    stats.effective_max_iters = max;
    stats.used_approx_equal = DomainHasApproxEqual<D>::value;
    stats.converged =
        result.stabilized && !stats.hit_limit && !stats.used_approx_equal;
    stats.domain_contract_checks_run = checks_run;
    stats.domain_contract_checks_failed = checks_run && !contract_ok;
    return {std::move(result.value), std::move(stats)};
  }
};

} // namespace npa

#endif // NPA_KLEENE_SOLVER_H
