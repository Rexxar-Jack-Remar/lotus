#ifndef NPA_LINEAR_SOLVERS_H
#define NPA_LINEAR_SOLVERS_H

/**
 * \file
 * \brief Solvers for the linearized equation system Df|ν(X) + δ = X.
 *
 * Each Newton round requires solving an LCFL equation system (when extend
 * is non-commutative). Supported strategies:
 * - SCC: global Tarjan SCC scheduling, with dependency-driven worklists inside
 *   each SCC (and optional layer-level parallelism across independent SCCs).
 * - Tensor (see TensorLinearSolve.h): lift to paired semiring, solve as
 *   left-linear (regular) system, project back (Reps et al. TOPLAS 2016).
 *
 * References: Esparza et al. (linearized system); Reps et al. (LCFL,
 * regularization via tensor product, Alg. 3.4).
 */

#include "Dataflow/NPA/Core/Base/Runtime.h"
#include "Dataflow/NPA/Core/IR/Diff.h"
#include "Dataflow/NPA/Core/IR/Eval.h"
#include "Dataflow/NPA/Core/IR/LCFLDetector.h"
#include "Utils/Parallel/ThreadPool.h"

#include <algorithm>
#include <atomic>
#include <deque>
#include <exception>
#include <set>

namespace npa {

namespace detail {

enum class SccStrategy {
  Direct,
  Worklist,
  Tensor,
};

enum class TensorFallbackReason {
  None,
  TensorUnavailable,
  TensorNotPaperAdmissible,
  TensorLawsNotValidated,
  ProjectionFragmentUnsupported,
};

struct LinearSccInfo {
  std::vector<int> members;
  bool has_self_loop = false;
  bool is_cyclic = false;
  std::size_t edge_count = 0;
  double density = 0.0;
  bool has_lcfl_structure = false;
  bool tensor_available = false;
  bool tensor_admissible = false;
  bool tensor_laws_validated = false;
  bool tensor_projection_sensitive = false;
  bool tensor_projection_fragment_supported = false;
  bool tensor_eligible = false;
  bool tensor_fallback = false;
  TensorFallbackReason tensor_fallback_reason = TensorFallbackReason::None;
  SccStrategy strategy = SccStrategy::Worklist;
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
  std::vector<std::vector<int>> layers;
  bool has_nontrivial_parallelism = false;
  std::size_t max_parallel_layer_width = 0;
  std::size_t parallel_scc_count = 0;
  std::size_t parallel_equation_volume = 0;
};

inline std::size_t parallel_task_grain_size(std::size_t total,
                                            std::size_t worker_count,
                                            std::size_t chunks_per_worker = 4) {
  if (total == 0 || worker_count <= 1)
    return 1;
  const std::size_t target_chunks =
      std::max<std::size_t>(1, worker_count * chunks_per_worker);
  return std::max<std::size_t>(1, (total + target_chunks - 1) / target_chunks);
}

inline std::size_t linear_parallel_min_equations(std::size_t worker_count) {
  return std::max<std::size_t>(128, worker_count * 64);
}

template <class D> struct LinearSccTaskResult {
  std::vector<DomVal<D>> values;
  long steps = 0;
  bool hit_limit = false;
  std::exception_ptr error;
};

template <class D>
std::deque<DomVal<D>> make_dense_value_buffer(
    const std::vector<DomVal<D>> &values) {
  return std::deque<DomVal<D>>(values.begin(), values.end());
}

template <class D>
LinearSccPlan<D>
build_linear_scc_plan(const std::vector<std::pair<Symbol, E1<D>>> &rhs) {
  LinearSccPlan<D> plan;
  const int n = static_cast<int>(rhs.size());
  for (int i = 0; i < n; ++i)
    plan.sym_to_idx[rhs[i].first] = i;

  plan.out_edges.resize(static_cast<std::size_t>(n));
  for (int i = 0; i < n; ++i) {
    std::unordered_set<Symbol> deps;
    DepFinder<D>::find(rhs[i].second, deps);
    for (const auto &dep : deps) {
      auto it = plan.sym_to_idx.find(dep);
      if (it != plan.sym_to_idx.end())
        plan.out_edges[static_cast<std::size_t>(i)].push_back(it->second);
    }
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

  for (int i = 0; i < n; ++i)
    if (index[i] == -1)
      tarjan(i);

  plan.sccs.assign(static_cast<std::size_t>(scc_count), {});
  for (int i = 0; i < n; ++i)
    plan.sccs[static_cast<std::size_t>(
                  plan.scc_id[static_cast<std::size_t>(i)])]
        .push_back(i);
  plan.infos.assign(static_cast<std::size_t>(scc_count), {});
  for (int sid = 0; sid < scc_count; ++sid)
    plan.infos[static_cast<std::size_t>(sid)].members =
        plan.sccs[static_cast<std::size_t>(sid)];

  for (int sid = 0; sid < scc_count; ++sid) {
    auto &info = plan.infos[static_cast<std::size_t>(sid)];
    for (int idx : info.members) {
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
    plan.has_nontrivial_parallelism =
        plan.has_nontrivial_parallelism || ready.size() > 1;
    if (ready.size() > 1) {
      plan.max_parallel_layer_width =
          std::max(plan.max_parallel_layer_width, ready.size());
      plan.parallel_scc_count += ready.size();
      for (int sid : ready) {
        plan.parallel_equation_volume +=
            plan.sccs[static_cast<std::size_t>(sid)].size();
      }
    }
    plan.layers.push_back(ready);

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

  return plan;
}

template <class D>
bool should_parallelize_linear_scc(bool verbose, const LinearSccPlan<D> &plan) {
  if (verbose)
    return false;
  if (!plan.has_nontrivial_parallelism)
    return false;
  const std::size_t worker_count = ThreadPool::get()->workerCount();
  if (worker_count <= 1)
    return false;
  if (plan.sym_to_idx.size() < linear_parallel_min_equations(worker_count))
    return false;
  const std::size_t min_layer_width = std::min<std::size_t>(worker_count, 4);
  if (plan.max_parallel_layer_width < min_layer_width)
    return false;
  if (plan.parallel_scc_count < worker_count * 2)
    return false;
  if (plan.parallel_equation_volume < worker_count * 16)
    return false;
  return true;
}

inline bool try_claim_linear_step(std::atomic<long> &shared_steps,
                                  long max_steps) {
  if (max_steps < 0) {
    shared_steps.fetch_add(1, std::memory_order_relaxed);
    return true;
  }
  long observed = shared_steps.load(std::memory_order_relaxed);
  while (observed < max_steps) {
    if (shared_steps.compare_exchange_weak(observed, observed + 1,
                                           std::memory_order_relaxed,
                                           std::memory_order_relaxed)) {
      return true;
    }
  }
  return false;
}

template <class D>
bool solve_linear_scc_serial_component(const LinearSccPlan<D> &plan,
                                       const std::vector<std::pair<Symbol, E1<D>>> &rhs,
                                       const std::vector<int> &scc,
                                       std::deque<DomVal<D>> &values,
                                       std::vector<DomVal<D>> &init,
                                       long &steps) {
  using V = DomVal<D>;
  const long max_steps = domain_max_linear_steps<D>();
  std::deque<int> worklist;
  std::vector<bool> in_queue(rhs.size(), false);
  auto lookup = [&](const Symbol &sym) -> const V & {
    const auto it = plan.sym_to_idx.find(sym);
    assert(it != plan.sym_to_idx.end() && "missing dense symbol index");
    return values[static_cast<std::size_t>(it->second)];
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

template <class D> struct DenseLinearSccOverlay {
  using V = DomVal<D>;

  const LinearSccPlan<D> &plan;
  const std::deque<V> &base_values;
  std::vector<int> local_positions;
  std::deque<V> local_values;

  DenseLinearSccOverlay(const LinearSccPlan<D> &solver_plan,
                        const std::deque<V> &base, const std::vector<int> &scc)
      : plan(solver_plan), base_values(base), local_positions(base.size(), -1),
        local_values(scc.size()) {
    for (std::size_t pos = 0; pos < scc.size(); ++pos) {
      const int idx = scc[pos];
      local_positions[static_cast<std::size_t>(idx)] =
          static_cast<int>(pos);
      local_values[pos] = base_values[static_cast<std::size_t>(idx)];
    }
  }

  const V &lookup(const Symbol &sym) const {
    const auto it = plan.sym_to_idx.find(sym);
    assert(it != plan.sym_to_idx.end() && "missing dense symbol index");
    const int idx = it->second;
    const int local_pos = local_positions[static_cast<std::size_t>(idx)];
    if (local_pos >= 0)
      return local_values[static_cast<std::size_t>(local_pos)];
    return base_values[static_cast<std::size_t>(idx)];
  }

  void update(int idx, V value) {
    const int local_pos = local_positions[static_cast<std::size_t>(idx)];
    assert(local_pos >= 0 && "updated symbol must be local to SCC");
    local_values[static_cast<std::size_t>(local_pos)] = std::move(value);
  }

  void appendResultValues(const std::vector<int> &scc, std::vector<V> &out) const {
    out.clear();
    out.reserve(scc.size());
    for (int idx : scc) {
      const int local_pos = local_positions[static_cast<std::size_t>(idx)];
      assert(local_pos >= 0 && "result symbol must be local to SCC");
      out.push_back(local_values[static_cast<std::size_t>(local_pos)]);
    }
  }
};

template <class D>
void solve_linear_scc_parallel_component(const LinearSccPlan<D> &plan,
                                         const std::vector<std::pair<Symbol, E1<D>>> &rhs,
                                         const std::vector<int> &scc,
                                         const std::deque<DomVal<D>> &base_values,
                                         std::atomic<long> &shared_steps,
                                         LinearSccTaskResult<D> &result) {
  using V = DomVal<D>;
  const long max_steps = domain_max_linear_steps<D>();
  DenseLinearSccOverlay<D> env(plan, base_values, scc);
  std::deque<int> worklist;
  std::vector<bool> in_queue(rhs.size(), false);
  for (int idx : scc) {
    worklist.push_back(idx);
    in_queue[static_cast<std::size_t>(idx)] = true;
  }

  while (!worklist.empty()) {
    const int idx = worklist.front();
    worklist.pop_front();
    in_queue[static_cast<std::size_t>(idx)] = false;

    if (!try_claim_linear_step(shared_steps, max_steps)) {
      result.hit_limit = true;
      break;
    }
    ++result.steps;

    const auto &entry = rhs[static_cast<std::size_t>(idx)];
    V new_val = I1<D>::evalWithLookup(
        false, [&](const Symbol &sym) -> const V & { return env.lookup(sym); },
        entry.second);
    if (!domain_equal<D>(env.lookup(entry.first), new_val)) {
      env.update(idx, new_val);
      for (int user : plan.intra_scc_users[static_cast<std::size_t>(idx)]) {
        if (!in_queue[static_cast<std::size_t>(user)]) {
          worklist.push_back(user);
          in_queue[static_cast<std::size_t>(user)] = true;
        }
      }
    }
  }

  env.appendResultValues(scc, result.values);
}

template <class D>
std::vector<DomVal<D>> solve_linear_scc_serial_from_plan(
    bool verbose, const std::vector<std::pair<Symbol, E1<D>>> &rhs,
    std::vector<DomVal<D>> init, const LinearSccPlan<D> &plan) {
  std::deque<DomVal<D>> values = make_dense_value_buffer<D>(init);

  long steps = 0;
  for (const auto &layer : plan.layers) {
    for (int sid : layer) {
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
  }

  if (verbose)
    std::cerr << "[linear-scc] steps=" << steps << " sccs=" << plan.sccs.size()
              << "\n";
  return init;
}

template <class D>
std::vector<DomVal<D>> solve_linear_scc_parallel_from_plan(
    bool verbose, const std::vector<std::pair<Symbol, E1<D>>> &rhs,
    std::vector<DomVal<D>> init, const LinearSccPlan<D> &plan) {
  (void)verbose;
  std::deque<DomVal<D>> values = make_dense_value_buffer<D>(init);

  std::atomic<long> shared_steps(0);
  ThreadPool *pool = ThreadPool::get();
  const auto execution_context = capture_execution_context<D>();
  for (const auto &layer : plan.layers) {
    if (layer.size() == 1) {
      const int sid = layer.front();
      LinearSccTaskResult<D> result;
      solve_linear_scc_parallel_component<D>(
          plan, rhs, plan.sccs[static_cast<std::size_t>(sid)], values,
          shared_steps, result);
      const auto &scc = plan.sccs[static_cast<std::size_t>(sid)];
      for (std::size_t pos = 0; pos < scc.size(); ++pos) {
        const int idx = scc[pos];
        values[static_cast<std::size_t>(idx)] = result.values[pos];
        init[static_cast<std::size_t>(idx)] = result.values[pos];
      }
      if (result.hit_limit) {
        npa_note_linear_limit_hit();
        return init;
      }
      continue;
    }

    std::vector<LinearSccTaskResult<D>> results(layer.size());
    const std::size_t grain_size =
        parallel_task_grain_size(layer.size(), pool->workerCount());
    pool->parallelFor<std::size_t>(0, layer.size(), grain_size,
                                   [&](std::size_t pos) {
                                     const int sid = layer[pos];
                                     ScopedExecutionContext<D> context_scope(
                                         execution_context);
                                     try {
                                       solve_linear_scc_parallel_component<D>(
                                           plan, rhs,
                                           plan.sccs[static_cast<std::size_t>(
                                               sid)],
                                           values, shared_steps, results[pos]);
                                     } catch (...) {
                                       results[pos].error =
                                           std::current_exception();
                                     }
                                   });

    bool hit_limit = false;
    for (std::size_t pos = 0; pos < layer.size(); ++pos) {
      if (results[pos].error)
        std::rethrow_exception(results[pos].error);
      const int sid = layer[pos];
      const auto &scc = plan.sccs[static_cast<std::size_t>(sid)];
      for (std::size_t value_pos = 0; value_pos < scc.size(); ++value_pos) {
        const int idx = scc[value_pos];
        values[static_cast<std::size_t>(idx)] = results[pos].values[value_pos];
        init[static_cast<std::size_t>(idx)] = results[pos].values[value_pos];
      }
      hit_limit = hit_limit || results[pos].hit_limit;
    }
    if (hit_limit) {
      npa_note_linear_limit_hit();
      return init;
    }
  }

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
  auto plan = detail::build_linear_scc_plan<D>(rhs);
  if (!detail::should_parallelize_linear_scc(verbose, plan))
    return detail::solve_linear_scc_serial_from_plan<D>(verbose, rhs,
                                                        std::move(init), plan);
  return detail::solve_linear_scc_parallel_from_plan<D>(verbose, rhs,
                                                        std::move(init), plan);
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

#endif // NPA_LINEAR_SOLVERS_H
