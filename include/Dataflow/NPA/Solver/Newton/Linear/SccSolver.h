#pragma once

/**
 * \file
 * \brief Ordinary inner solvers for the linearized Newton/NPA system.
 *
 * Each Newton round of JACM-style Newton/NPA requires solving the linearized
 * system `Df|ν(X) + δ = X`. This header contains the non-tensor backends and
 * the SCC planning/scheduling logic around them.
 *
 * Supported strategies here:
 * - SCC: global Tarjan SCC scheduling, with dependency-driven worklists inside
 *   each SCC.
 * - Naive: synchronized fixpoint iteration over the whole linearized system.
 *
 * `AdaptiveScc` also reuses the SCC planning from this file, but it may
 * delegate specific tensor-eligible SCCs to `TensorProduct.h`. The full
 * tensor-product backend itself lives there, not here.
 *
 * References:
 * - Esparza et al. (JACM): linearized system.
 * - Reps et al. (TOPLAS 2016): tensor regularization, implemented separately
 *   in `TensorProduct.h`.
 */

#include "Dataflow/NPA/Core/Expr/Eval.h"
#include "Dataflow/NPA/Solver/EquationSystem.h"
#include "Dataflow/NPA/Solver/Newton/Linear/ExpressionAnalysis.h"
#include "Dataflow/NPA/Solver/SolveContext.h"

#include <algorithm>
#include <deque>
#include <set>

namespace npa {

namespace detail {

struct LinearSccInfo {
  std::vector<int> members;
  bool has_self_loop = false;
  bool is_cyclic = false;
  std::size_t edge_count = 0;
  double density = 0.0;
  bool has_lcfl_structure = false;
};

template <class D> struct LinearSccPlan {
  std::unordered_map<Symbol, int> sym_to_idx;
  std::vector<std::vector<int>> out_edges;
  std::vector<std::vector<int>> intra_scc_users;
  std::vector<int> scc_id;
  std::vector<std::vector<int>> sccs;
  std::vector<LinearSccInfo> infos;
  std::vector<std::vector<int>> cond_successors;
  std::vector<std::vector<int>> cond_predecessors;
  std::vector<int> scc_order;
};

template <class D>
std::deque<DomVal<D>>
make_dense_value_buffer(const std::vector<DomVal<D>> &values) {
  return std::deque<DomVal<D>>(values.begin(), values.end());
}

template <class D>
void complete_linear_scc_plan(const std::vector<std::pair<Symbol, E1<D>>> &rhs,
                              LinearSccPlan<D> &plan,
                              bool inspect_lcfl_structure = true) {
  const int n = static_cast<int>(rhs.size());
  const int scc_count = static_cast<int>(plan.sccs.size());
  plan.infos.assign(plan.sccs.size(), {});
  for (int sid = 0; sid < scc_count; ++sid) {
    plan.infos[static_cast<std::size_t>(sid)].members =
        plan.sccs[static_cast<std::size_t>(sid)];
  }

  for (int sid = 0; sid < scc_count; ++sid) {
    auto &info = plan.infos[static_cast<std::size_t>(sid)];
    for (int idx : info.members) {
      if (inspect_lcfl_structure) {
        info.has_lcfl_structure =
            info.has_lcfl_structure ||
            LCFLDetector<D>::has_lcfl_structure(
                rhs[static_cast<std::size_t>(idx)].second);
      }
      for (int dep : plan.out_edges[static_cast<std::size_t>(idx)]) {
        if (plan.scc_id[static_cast<std::size_t>(dep)] != sid)
          continue;
        ++info.edge_count;
        if (dep == idx)
          info.has_self_loop = true;
      }
    }
    info.is_cyclic = info.members.size() > 1 || info.has_self_loop;
    const double denom =
        static_cast<double>(info.members.size() * info.members.size());
    info.density =
        denom > 0.0 ? static_cast<double>(info.edge_count) / denom : 0.0;
  }

  plan.intra_scc_users.assign(static_cast<std::size_t>(n), {});
  for (int user = 0; user < n; ++user) {
    const int user_sid = plan.scc_id[static_cast<std::size_t>(user)];
    for (int dep : plan.out_edges[static_cast<std::size_t>(user)]) {
      if (plan.scc_id[static_cast<std::size_t>(dep)] == user_sid)
        plan.intra_scc_users[static_cast<std::size_t>(dep)].push_back(user);
    }
  }

  std::vector<std::set<int>> succ_sets(static_cast<std::size_t>(scc_count));
  std::vector<std::set<int>> pred_sets(static_cast<std::size_t>(scc_count));
  for (int v = 0; v < n; ++v) {
    for (int w : plan.out_edges[static_cast<std::size_t>(v)]) {
      int dependency_sid = plan.scc_id[static_cast<std::size_t>(w)];
      int user_sid = plan.scc_id[static_cast<std::size_t>(v)];
      if (dependency_sid == user_sid)
        continue;
      succ_sets[static_cast<std::size_t>(dependency_sid)].insert(user_sid);
      pred_sets[static_cast<std::size_t>(user_sid)].insert(dependency_sid);
    }
  }

  plan.cond_successors.assign(static_cast<std::size_t>(scc_count), {});
  plan.cond_predecessors.assign(static_cast<std::size_t>(scc_count), {});
  for (int sid = 0; sid < scc_count; ++sid) {
    plan.cond_successors[static_cast<std::size_t>(sid)].assign(
        succ_sets[static_cast<std::size_t>(sid)].begin(),
        succ_sets[static_cast<std::size_t>(sid)].end());
    plan.cond_predecessors[static_cast<std::size_t>(sid)].assign(
        pred_sets[static_cast<std::size_t>(sid)].begin(),
        pred_sets[static_cast<std::size_t>(sid)].end());
  }

  std::vector<int> indegree(static_cast<std::size_t>(scc_count), 0);
  for (int sid = 0; sid < scc_count; ++sid)
    indegree[static_cast<std::size_t>(sid)] = static_cast<int>(
        plan.cond_predecessors[static_cast<std::size_t>(sid)].size());

  std::vector<int> ready;
  ready.reserve(static_cast<std::size_t>(scc_count));
  for (int sid = 0; sid < scc_count; ++sid)
    if (indegree[static_cast<std::size_t>(sid)] == 0)
      ready.push_back(sid);

  while (!ready.empty()) {
    std::sort(ready.begin(), ready.end());
    plan.scc_order.insert(plan.scc_order.end(), ready.begin(), ready.end());

    std::vector<int> next_ready;
    for (int sid : ready) {
      for (int succ : plan.cond_successors[static_cast<std::size_t>(sid)]) {
        int &succ_indegree = indegree[static_cast<std::size_t>(succ)];
        --succ_indegree;
        if (succ_indegree == 0)
          next_ready.push_back(succ);
      }
    }
    ready.swap(next_ready);
  }
}

template <class D>
LinearSccPlan<D>
build_linear_scc_plan(const std::vector<std::pair<Symbol, E1<D>>> &rhs) {
  LinearSccPlan<D> plan;
  const int n = static_cast<int>(rhs.size());
  const auto validated = validate_linear_equation_system<D>(rhs);
  plan.sym_to_idx.reserve(validated.symbol_to_index.size());
  for (const auto &entry : validated.symbol_to_index)
    plan.sym_to_idx.emplace(entry.first, static_cast<int>(entry.second));

  plan.out_edges.resize(static_cast<std::size_t>(n));
  for (int i = 0; i < n; ++i) {
    const auto &dependencies =
        validated.dependencies[static_cast<std::size_t>(i)];
    auto &edges = plan.out_edges[static_cast<std::size_t>(i)];
    edges.reserve(dependencies.size());
    for (unsigned dependency : dependencies)
      edges.push_back(static_cast<int>(dependency));
  }

  std::vector<int> index(n, -1), low(n, -1);
  plan.scc_id.assign(static_cast<std::size_t>(n), -1);
  std::vector<int> stack;
  stack.reserve(static_cast<std::size_t>(n));
  int next_index = 0;
  int scc_count = 0;

  std::function<void(int)> tarjan = [&](int v) {
    index[v] = low[v] = next_index++;
    stack.push_back(v);
    for (int w : plan.out_edges[static_cast<std::size_t>(v)]) {
      if (index[w] == -1) {
        tarjan(w);
        low[v] = std::min(low[v], low[w]);
      } else if (plan.scc_id[static_cast<std::size_t>(w)] == -1) {
        low[v] = std::min(low[v], index[w]);
      }
    }
    if (low[v] == index[v]) {
      for (;;) {
        int u = stack.back();
        stack.pop_back();
        plan.scc_id[static_cast<std::size_t>(u)] = scc_count;
        if (u == v)
          break;
      }
      ++scc_count;
    }
  };

  for (int i = 0; i < n; ++i) {
    if (index[i] == -1)
      tarjan(i);
  }

  plan.sccs.assign(static_cast<std::size_t>(scc_count), {});
  for (int i = 0; i < n; ++i) {
    plan.sccs[static_cast<std::size_t>(
                  plan.scc_id[static_cast<std::size_t>(i)])]
        .push_back(i);
  }
  complete_linear_scc_plan(rhs, plan);
  return plan;
}

/// Build a solve plan for an already validated reduced system. Sparse round
/// construction supplies exact dependencies and a cached SCC partition, so
/// this path does not rescan the materialized Exp1 DAG or rerun Tarjan.
template <class D>
LinearSccPlan<D> build_linear_scc_plan_from_partition(
    const std::vector<std::pair<Symbol, E1<D>>> &rhs,
    const std::vector<std::vector<unsigned>> &dependencies,
    const std::vector<std::vector<unsigned>> &partition,
    bool inspect_lcfl_structure = true) {
  if (dependencies.size() != rhs.size())
    throw InvalidEquationSystemError(
        "linear equation system and dependency plan differ in size");

  LinearSccPlan<D> plan;
  const std::size_t count = rhs.size();
  plan.sym_to_idx.reserve(count);
  for (std::size_t index = 0; index < count; ++index) {
    if (!plan.sym_to_idx.emplace(rhs[index].first, static_cast<int>(index))
             .second) {
      throw InvalidEquationSystemError("duplicate equation LHS symbol");
    }
  }

  plan.out_edges.resize(count);
  for (std::size_t user = 0; user < count; ++user) {
    auto &edges = plan.out_edges[user];
    edges.reserve(dependencies[user].size());
    for (unsigned dependency : dependencies[user]) {
      if (dependency >= count)
        throw InvalidEquationSystemError("invalid reduced dependency index");
      edges.push_back(static_cast<int>(dependency));
    }
  }

  plan.scc_id.assign(count, -1);
  plan.sccs.reserve(partition.size());
  for (const auto &component : partition) {
    const int sid = static_cast<int>(plan.sccs.size());
    plan.sccs.emplace_back();
    auto &members = plan.sccs.back();
    members.reserve(component.size());
    for (unsigned member : component) {
      if (member >= count || plan.scc_id[member] != -1)
        throw InvalidEquationSystemError("invalid reduced SCC partition");
      plan.scc_id[member] = sid;
      members.push_back(static_cast<int>(member));
    }
    if (members.empty())
      plan.sccs.pop_back();
  }
  for (int sid : plan.scc_id) {
    if (sid == -1)
      throw InvalidEquationSystemError("incomplete reduced SCC partition");
  }

  complete_linear_scc_plan(rhs, plan, inspect_lcfl_structure);
  return plan;
}

template <class D>
bool solve_linear_scc_serial_component(
    const LinearSccPlan<D> &plan,
    const std::vector<std::pair<Symbol, E1<D>>> &rhs,
    const std::vector<int> &scc, std::deque<DomVal<D>> &values,
    std::vector<DomVal<D>> &init, long &steps) {
  using V = DomVal<D>;
  const long max_steps = domain_max_linear_steps<D>();
  std::deque<int> worklist;
  std::vector<bool> in_queue(rhs.size(), false);
  auto lookup = [&](const Symbol &sym) -> const V & {
    return values[static_cast<std::size_t>(plan.sym_to_idx.at(sym))];
  };
  for (int idx : scc) {
    worklist.push_back(idx);
    in_queue[static_cast<std::size_t>(idx)] = true;
  }

  while (!worklist.empty()) {
    const int idx = worklist.front();
    worklist.pop_front();
    in_queue[static_cast<std::size_t>(idx)] = false;

    ++steps;
    if (max_steps >= 0 && steps > max_steps)
      return false;

    V new_val = I1<D>::evalWithLookup(
        false, lookup, rhs[static_cast<std::size_t>(idx)].second);
    if (!domain_equal<D>(values[static_cast<std::size_t>(idx)], new_val)) {
      values[static_cast<std::size_t>(idx)] = new_val;
      init[static_cast<std::size_t>(idx)] = new_val;
      for (int user : plan.intra_scc_users[static_cast<std::size_t>(idx)]) {
        if (!in_queue[static_cast<std::size_t>(user)]) {
          worklist.push_back(user);
          in_queue[static_cast<std::size_t>(user)] = true;
        }
      }
    }
  }
  return true;
}

template <class D>
std::vector<DomVal<D>> solve_linear_scc_serial_from_plan(
    bool verbose, const std::vector<std::pair<Symbol, E1<D>>> &rhs,
    std::vector<DomVal<D>> init, const LinearSccPlan<D> &plan) {
  std::deque<DomVal<D>> values = make_dense_value_buffer<D>(init);

  long steps = 0;
  for (int sid : plan.scc_order) {
    if (!solve_linear_scc_serial_component<D>(
            plan, rhs, plan.sccs[static_cast<std::size_t>(sid)], values, init,
            steps)) {
      npa_note_linear_limit_hit();
      if (verbose) {
        std::cerr << "[linear-scc] hit max_linear_steps="
                  << domain_max_linear_steps<D>() << "\n";
      }
      return init;
    }
  }

  if (verbose)
    std::cerr << "[linear-scc] steps=" << steps << " sccs=" << plan.sccs.size()
              << "\n";
  return init;
}

} // namespace detail

/// Solve linear system by SCC: Tarjan to find SCCs, topological order on
/// SCCs, then dependency-driven worklist solving within each SCC.
template <class D>
std::vector<DomVal<D>>
solve_linear_scc_impl(bool verbose,
                      const std::vector<std::pair<Symbol, E1<D>>> &rhs,
                      std::vector<DomVal<D>> init) {
  if (init.size() != rhs.size())
    throw InvalidEquationSystemError(
        "linear equation system and initial values differ in size");
  auto plan = detail::build_linear_scc_plan<D>(rhs);
  return detail::solve_linear_scc_serial_from_plan<D>(verbose, rhs,
                                                      std::move(init), plan);
}

/// Solve linear system via tensor product (Reps et al. Alg. 3.4): convert
/// LCFL system to left-linear system over paired semiring, solve there,
/// project back. Implemented in TensorProduct.h.
template <class D>
std::vector<DomVal<D>>
solve_linear_tensor_impl(bool verbose,
                         const std::vector<std::pair<Symbol, E1<D>>> &rhs,
                         std::vector<DomVal<D>> init);

/// True if any equation has LCFL structure (Concat or Star). The tensor
/// strategy is only considered when this holds; the tensor solver may still
/// fall back to the SCC solver if regularization preconditions are not met.
template <class D>
inline bool
system_has_lcfl_structure(const std::vector<std::pair<Symbol, E1<D>>> &rhs) {
  for (const auto &p : rhs)
    if (LCFLDetector<D>::has_lcfl_structure(p.second))
      return true;
  return false;
}

} // namespace npa

