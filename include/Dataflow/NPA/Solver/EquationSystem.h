#ifndef NPA_EQUATION_SYSTEM_H
#define NPA_EQUATION_SYSTEM_H

/**
 * \file
 * \brief Shared fixed-point driver for whole equation-system solvers.
 *
 * This is the common outer iteration loop reused by:
 * - `KleeneSolver<D>` for direct Kleene solving of `X = f(X)`
 * - `NPASolver<D>` for Newtonian Program Analysis on `X = f(X)`
 *
 * It is intentionally not the main public entry point. Users should include
 * `KleeneSolver.h` or `NPASolver.h` depending on which equation-system solver
 * want to use.
 */

#include "Dataflow/NPA/Core/Base/Runtime.h"
#include "Dataflow/NPA/Core/Expr/Expressions.h"

#include <chrono>

namespace npa {

template <class D, class ITER> struct EquationSystemSolver {
  using V = DomVal<D>;
  using Eqn = std::pair<Symbol, E0<D>>;
  static std::pair<std::vector<std::pair<Symbol, V>>, Stat>
  solve(const std::vector<Eqn> &eqns, bool verbose = false, int max = -1,
        LinearStrategy linStrat = LinearStrategy::SCC,
        DomainContractMode contractMode = DomainContractMode::Off) {
    NPA_REQUIRE_DOMAIN(D);
    ApproximationSourceCollector approximation_collector;
    ScopedApproximationSourceCollector collector_scope(approximation_collector);
    AdaptiveSccSolveCollector adaptive_scc_collector;
    ScopedAdaptiveSccSolveCollector adaptive_scc_scope(adaptive_scc_collector);
    npa_reset_limit_hit();
    npa_reset_adaptive_scc_stats();
    bool contractOk = true;
    const bool checksRun = contractMode == DomainContractMode::BasicChecks;
    if (checksRun) {
      contractOk = run_basic_domain_contract_checks<D>(verbose);
    }
    std::vector<std::pair<Symbol, V>> cur = ITER::init(eqns);
    auto tic = std::chrono::high_resolution_clock::now();
    int it = 0;
    bool converged = false;
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
        converged = true;
        if (verbose)
          std::cerr << "[conv] " << it << "\n";
        break;
      }
    }
    const bool hit_outer_limit = !converged && max >= 0 && it >= max;
    if (hit_outer_limit) {
      npa_note_outer_limit_hit();
      if (verbose)
        std::cerr << "[conv] hit outer iteration cap=" << max << "\n";
    }
    auto toc = std::chrono::high_resolution_clock::now();
    Stat st;
    st.iters = it;
    st.time = std::chrono::duration<double>(toc - tic).count();
    st.hit_limit = npa_limit_hit();
    st.hit_outer_limit = npa_hit_outer_limit();
    st.hit_linear_limit = npa_hit_linear_limit();
    st.hit_fixpoint_limit = npa_hit_fixpoint_limit();
    st.equation_count = static_cast<int>(eqns.size());
    st.requested_max_iters = max;
    st.effective_max_iters = max;
    st.linear_strategy = linStrat;
    st.used_approx_equal = DomainHasApproxEqual<D>::value;
    const auto adaptive_stats = npa_adaptive_scc_solve_stats();
    st.adaptive_scc_used = adaptive_stats.used;
    st.adaptive_scc_direct_count = adaptive_stats.direct_count;
    st.adaptive_scc_worklist_count = adaptive_stats.worklist_count;
    st.adaptive_scc_tensor_count = adaptive_stats.tensor_count;
    st.adaptive_scc_tensor_fallback_count =
        adaptive_stats.tensor_fallback_count;
    st.converged = converged && !st.hit_limit && !st.used_approx_equal;
    st.domain_contract_checks_run = checksRun;
    st.domain_contract_checks_failed = checksRun && !contractOk;
    return {cur, st};
  }
};

} // namespace npa

#endif // NPA_EQUATION_SYSTEM_H
