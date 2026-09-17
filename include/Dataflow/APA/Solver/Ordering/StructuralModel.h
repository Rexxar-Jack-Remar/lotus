#ifndef DATAFLOW_APA_ENGINES_ELIMINATIONORDER_H_
#define DATAFLOW_APA_ENGINES_ELIMINATIONORDER_H_

// Cost-aware elimination ordering for the state-elimination engine (paper's
// "Order" configuration
//
// Eliminating a vertex k performs, for all i, j:
//     M[i][j] |= M[i][k] · M[k][k]* · M[k][j].
// The number and size of the intermediate updates is driven by the
// predecessor–successor product deg_in(k) · deg_out(k): k contributes one
// update per (predecessor, successor) pair and adds a fill edge i→j for each
// such pair. Choosing the pivot order therefore controls peak construction
// cost, but never the final path language. Equal interpretations additionally
// require a language-invariant client algebra (e.g. a quantale).
//
// This header is a pure combinatorial model over a boolean elimination graph:
// it never touches the expression matrix, so it is LLVM-free and unit-testable
// in isolation. computeCostAwareOrder yields the greedy minimum-product
// permutation; simulateCost scores ANY permutation (including the baseline's)
// so the evaluation can compare orders (RQ3).

#include <cstddef>
#include <unordered_set>
#include <vector>

namespace elimination {
namespace order {

// Directed graph over node indices [0, n). No self-loops are stored (they do
// not affect the predecessor–successor product) — build() and fill both skip
// i == j.
struct EliminationGraph {
  std::size_t n = 0;
  std::vector<std::unordered_set<std::size_t>> preds;
  std::vector<std::unordered_set<std::size_t>> succs;

  explicit EliminationGraph(std::size_t N = 0) : n(N), preds(N), succs(N) {}

  void addEdge(std::size_t i, std::size_t j) {
    if (i == j) {
      return;
    }
    if (succs[i].insert(j).second) {
      preds[j].insert(i);
    }
  }

  std::size_t totalEdges() const {
    std::size_t e = 0;
    for (const auto &s : succs) {
      e += s.size();
    }
    return e;
  }
};

// Aggregate cost of running elimination in a given order over a boolean graph.
struct OrderCost {
  // Σ_k deg_in(k)·deg_out(k) at the moment k is eliminated — the paper's
  // predecessor–successor product summed over the elimination (a count-based
  // proxy for total intermediate updates / raw-DAG construction work).
  double totalProduct = 0.0;
  // Peak number of edges in the (fill-augmented) elimination graph at any point
  // — a proxy for peak construction density / retained intermediate size.
  std::size_t peakFill = 0;
};

namespace detail {

// Eliminate vertex k in place: add a fill edge from every live predecessor to
// every live successor, then detach k. Returns the number of NEW fill edges
// added (edges that were not already present). `removed` is set to the number
// of edges incident to k that are deleted.
inline std::size_t eliminate(EliminationGraph &g, std::size_t k,
                             std::size_t &removed) {
  std::size_t added = 0;
  for (std::size_t i : g.preds[k]) {
    for (std::size_t j : g.succs[k]) {
      if (i == j) {
        continue;
      }
      if (g.succs[i].insert(j).second) {
        g.preds[j].insert(i);
        ++added;
      }
    }
  }
  removed = g.preds[k].size() + g.succs[k].size();
  for (std::size_t i : g.preds[k]) {
    g.succs[i].erase(k);
  }
  for (std::size_t j : g.succs[k]) {
    g.preds[j].erase(k);
  }
  g.preds[k].clear();
  g.succs[k].clear();
  return added;
}

} // namespace detail

// Greedy minimum-product elimination order. At each step, among the not-yet-
// eliminated vertices, pick the one minimizing deg_in·deg_out on the current
// (fill-augmented) graph; ties broken by deg_in+deg_out, then by index — so the
// result is fully deterministic and independent of hash-set iteration order.
// Returns a permutation of [0, n).
inline std::vector<std::size_t> computeCostAwareOrder(const EliminationGraph &g0) {
  const std::size_t n = g0.n;
  std::vector<std::size_t> order;
  order.reserve(n);
  if (n == 0) {
    return order;
  }

  EliminationGraph g = g0; // working copy (mutated by elimination)
  std::vector<bool> dead(n, false);

  for (std::size_t step = 0; step < n; ++step) {
    std::size_t best = n;
    std::size_t bestProd = 0, bestDeg = 0;
    for (std::size_t k = 0; k < n; ++k) {
      if (dead[k]) {
        continue;
      }
      const std::size_t in = g.preds[k].size();
      const std::size_t out = g.succs[k].size();
      const std::size_t prod = in * out;
      const std::size_t deg = in + out;
      if (best == n || prod < bestProd ||
          (prod == bestProd && deg < bestDeg)) {
        best = k;
        bestProd = prod;
        bestDeg = deg;
      }
    }
    std::size_t removed = 0;
    detail::eliminate(g, best, removed);
    dead[best] = true;
    order.push_back(best);
  }
  return order;
}

// Score an arbitrary elimination order on `g0` (a fresh copy is used, so `g0`
// is not modified). Any node in `order` that is not a valid live index is
// skipped defensively. Nodes missing from `order` are not scored.
inline OrderCost simulateCost(const EliminationGraph &g0,
                              const std::vector<std::size_t> &order) {
  OrderCost cost;
  EliminationGraph g = g0;
  std::vector<bool> dead(g.n, false);
  std::size_t edges = g.totalEdges();
  cost.peakFill = edges;

  for (std::size_t k : order) {
    if (k >= g.n || dead[k]) {
      continue;
    }
    const std::size_t in = g.preds[k].size();
    const std::size_t out = g.succs[k].size();
    cost.totalProduct += static_cast<double>(in) * static_cast<double>(out);

    std::size_t removed = 0;
    const std::size_t added = detail::eliminate(g, k, removed);
    // Transient density: fill added while k's own edges are still present.
    const std::size_t transient = edges + added;
    if (transient > cost.peakFill) {
      cost.peakFill = transient;
    }
    edges = transient - removed;
    dead[k] = true;
  }
  return cost;
}

} // namespace order
} // namespace elimination

#endif // DATAFLOW_APA_ENGINES_ELIMINATIONORDER_H_
