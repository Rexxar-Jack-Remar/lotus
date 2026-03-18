#ifndef NPA_LINEAR_SOLVERS_H
#define NPA_LINEAR_SOLVERS_H

/**
 * \file
 * \brief Solvers for the linearized equation system Df|ν(X) + δ = X.
 *
 * Each Newton round requires solving an LCFL equation system (when extend
 * is non-commutative). Three strategies:
 * - Worklist: chaotic iteration with dependency-driven worklist (conventional
 *   fixpoint on the linear system).
 * - SCC: Tarjan SCCs, solve in topological order, fixpoint per SCC (faster
 *   when the dependency graph has nontrivial SCCs).
 * - Tensor (see TensorLinearSolve.h): lift to paired semiring, solve as
 *   left-linear (regular) system, project back (Reps et al. TOPLAS 2016).
 *
 * References: Esparza et al. (linearized system); Reps et al. (LCFL,
 * regularization via tensor product, Alg. 3.4).
 */

#include "Dataflow/NPA/Core/Diff.h"
#include "Dataflow/NPA/Core/Eval.h"
#include "Dataflow/NPA/Core/LCFLDetector.h"

namespace npa {

/// Solve linear system (RHS = vector of Exp1) by worklist: repeatedly
/// evaluate RHS under current env and push dependents until stable.
template <class D>
std::vector<DomVal<D>>
solve_linear_worklist_impl(bool verbose,
                           const std::vector<std::pair<Symbol, E1<D>>> &rhs,
                           std::vector<DomVal<D>> init) {
  using V = DomVal<D>;
  std::unordered_map<Symbol, int> sym_to_idx;
  std::unordered_map<Symbol, V> env;
  for (int i = 0; i < (int)rhs.size(); ++i) {
    sym_to_idx[rhs[i].first] = i;
    env[rhs[i].first] = init[i];
  }
  std::vector<std::vector<int>> users(rhs.size());
  for (int i = 0; i < (int)rhs.size(); ++i) {
    std::unordered_set<Symbol> deps;
    DepFinder<D>::find(rhs[i].second, deps);
    for (const auto &d : deps)
      if (sym_to_idx.count(d))
        users[sym_to_idx[d]].push_back(i);
  }
  std::deque<int> worklist;
  std::vector<bool> in_queue(rhs.size(), false);
  for (int i = 0; i < (int)rhs.size(); ++i) {
    worklist.push_back(i);
    in_queue[i] = true;
  }
  long steps = 0;
  const long max_steps = domain_max_linear_steps<D>();
  while (!worklist.empty()) {
    int idx = worklist.front();
    worklist.pop_front();
    in_queue[idx] = false;
    steps++;
    if (max_steps >= 0 && steps > max_steps) {
      npa_note_linear_limit_hit();
      if (verbose)
        std::cerr << "[linear-wl] hit max_linear_steps=" << max_steps << "\n";
      break;
    }
    V new_val = I1<D>::eval(false, env, rhs[idx].second);
    if (!domain_equal<D>(env[rhs[idx].first], new_val)) {
      env[rhs[idx].first] = new_val;
      init[idx] = new_val;
      for (int u : users[idx])
        if (!in_queue[u]) {
          worklist.push_back(u);
          in_queue[u] = true;
        }
    }
  }
  if (verbose)
    std::cerr << "[linear-wl] steps=" << steps << "\n";
  return init;
}

/// Solve linear system by SCC: Tarjan to find SCCs, topological order on
/// SCCs, then fixpoint iteration within each SCC (faster for cyclic deps).
template <class D>
std::vector<DomVal<D>>
solve_linear_scc_impl(bool verbose,
                      const std::vector<std::pair<Symbol, E1<D>>> &rhs,
                      std::vector<DomVal<D>> init) {
  using V = DomVal<D>;
  const int n = static_cast<int>(rhs.size());
  std::unordered_map<Symbol, int> sym_to_idx;
  for (int i = 0; i < n; ++i)
    sym_to_idx[rhs[i].first] = i;
  std::vector<std::vector<int>> out_edges(n);
  for (int i = 0; i < n; ++i) {
    std::unordered_set<Symbol> deps;
    DepFinder<D>::find(rhs[i].second, deps);
    for (const auto &d : deps) {
      auto it = sym_to_idx.find(d);
      if (it != sym_to_idx.end())
        out_edges[i].push_back(it->second);
    }
  }
  std::vector<int> index(n, -1), low(n, -1), scc_id(n, -1);
  std::vector<int> stack;
  int idx = 0, scc_count = 0;
  std::function<void(int)> tarjan = [&](int v) {
    index[v] = low[v] = idx++;
    stack.push_back(v);
    for (int w : out_edges[v]) {
      if (index[w] == -1) {
        tarjan(w);
        low[v] = std::min(low[v], low[w]);
      } else if (scc_id[w] == -1) {
        low[v] = std::min(low[v], index[w]);
      }
    }
    if (low[v] == index[v]) {
      for (;;) {
        int u = stack.back();
        stack.pop_back();
        scc_id[u] = scc_count;
        if (u == v)
          break;
      }
      ++scc_count;
    }
  };
  for (int i = 0; i < n; ++i)
    if (index[i] == -1)
      tarjan(i);
  std::vector<std::vector<int>> sccs(scc_count);
  for (int i = 0; i < n; ++i)
    sccs[scc_id[i]].push_back(i);
  std::vector<std::vector<int>> rev_cond(scc_count);
  for (int v = 0; v < n; ++v)
    for (int w : out_edges[v])
      if (scc_id[v] != scc_id[w])
        rev_cond[scc_id[w]].push_back(scc_id[v]);
  std::vector<int> rev_in_degree(scc_count, 0);
  for (int a = 0; a < scc_count; ++a)
    for (int b : rev_cond[a])
      ++rev_in_degree[b];
  std::deque<int> q;
  for (int i = 0; i < scc_count; ++i)
    if (rev_in_degree[i] == 0)
      q.push_back(i);
  std::vector<int> scc_order;
  scc_order.reserve(scc_count);
  while (!q.empty()) {
    int a = q.front();
    q.pop_front();
    scc_order.push_back(a);
    for (int b : rev_cond[a])
      if (--rev_in_degree[b] == 0)
        q.push_back(b);
  }
  std::unordered_map<Symbol, V> env;
  for (int i = 0; i < n; ++i)
    env[rhs[i].first] = init[i];
  long steps = 0;
  const long max_steps = domain_max_linear_steps<D>();
  for (int sid : scc_order) {
    const auto &scc = sccs[sid];
    for (;;) {
      bool stable = true;
      for (int i : scc) {
        V new_val = I1<D>::eval(false, env, rhs[i].second);
        if (!domain_equal<D>(env[rhs[i].first], new_val)) {
          env[rhs[i].first] = new_val;
          init[i] = new_val;
          stable = false;
        }
        ++steps;
        if (max_steps >= 0 && steps > max_steps) {
          npa_note_linear_limit_hit();
          if (verbose)
            std::cerr << "[linear-scc] hit max_linear_steps=" << max_steps
                      << "\n";
          return init;
        }
      }
      if (stable)
        break;
    }
  }
  if (verbose)
    std::cerr << "[linear-scc] steps=" << steps << " sccs=" << scc_count
              << "\n";
  return init;
}

/// Solve linear system via tensor product (Reps et al. Alg. 3.4): convert
/// LCFL system to left-linear system over paired semiring, solve there,
/// project back. Implemented in TensorLinearSolve.h.
template <class D>
std::vector<DomVal<D>>
solve_linear_tensor_impl(bool verbose,
                         const std::vector<std::pair<Symbol, E1<D>>> &rhs,
                         std::vector<DomVal<D>> init);

/// True if any equation has LCFL structure (Concat or Star). The tensor
/// strategy is only considered when this holds; the tensor solver may still
/// fall back to worklist if regularization preconditions are not met.
template <class D>
inline bool
system_has_lcfl_structure(const std::vector<std::pair<Symbol, E1<D>>> &rhs) {
  for (const auto &p : rhs)
    if (LCFLDetector<D>::has_lcfl_structure(p.second))
      return true;
  return false;
}

} // namespace npa

#endif // NPA_LINEAR_SOLVERS_H
