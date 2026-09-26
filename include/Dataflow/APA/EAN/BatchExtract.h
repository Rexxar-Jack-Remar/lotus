#pragma once

// Reuse-aware batch extraction (paper §III.D, Algorithm 2): pick one e-node per
// e-class to minimize the shared-DAG objective (Eq. 5), not the additive tree
// cost. Two ingredients:
//
//  * cycleSafeExtract — a bounded relaxation (Bellman–Ford style) that assigns
//    each class its cheapest node, where a node is eligible only once ALL its
//    children already have a finite cost. This guarantees the selected DAG is
//    finite even when the e-graph is cyclic (star unfolding, M5). On an acyclic
//    graph it degenerates to ordinary bottom-up extraction.
//
//  * the reuse loop — starting from the tree solution, repeatedly count
//    references in the current selected DAG and re-extract with a sublinear
//    discount on multiply-referenced classes, biasing the extractor toward
//    reuse. Each candidate is scored by the TRUE Eq. 5 objective and the best
//    is kept, so an inaccurate discount can never make the result worse than
//    the initial tree extraction (paper's safety net; not a claim of DAG
//    optimality).

#include <cstdint>
#include <deque>
#include <functional>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "Dataflow/APA/EAN/Canonical.h"
#include "Dataflow/APA/EAN/CostFn.h"
#include "Dataflow/APA/EAN/CostModel.h"
#include "Dataflow/APA/EAN/ExtractOptions.h"
#include "Dataflow/APA/EAN/PathLang.h"

namespace elimination {
namespace ean {

// The chosen representative e-node per (canonical) e-class id.
using Selection = std::unordered_map<std::uint32_t, PathLang>;

struct ExtractResult {
  Selection chosen;
  double dagCost = 0.0;
};

namespace detail {

constexpr double kInf = std::numeric_limits<double>::infinity();

struct RelaxState {
  Selection chosen;
  std::unordered_map<std::uint32_t, double> cost;
  double at(std::uint32_t c) const {
    auto it = cost.find(c);
    return it == cost.end() ? kInf : it->second;
  }
};

// Cycle-safe per-class cheapest-node relaxation. `wNode(node)` is the node's own
// weight; `disc(childClassId)` scales a child's cost when a parent consumes it
// (used to reward reuse; 1.0 = no discount).
//
// Uses a parent-pointer work queue (egg-style) instead of a whole-graph
// rescan-to-fixpoint: a class is (re)relaxed only when it is first seeded or
// when one of its children's cost improved. Node costs are monotonically
// non-increasing, so the queue converges to the same least fixpoint the naive
// `for pass: for all classes` relaxation reached with a sufficient pass count
// (the sole caller uses numberOfClasses()+1, which always suffices) — the chosen
// representative and cost are byte-for-byte identical. `passes` is retained for
// API compatibility and now bounds only pathological non-convergence.
template <typename WNodeFn, typename DiscFn>
RelaxState cycleSafeExtract(Graph &g, WNodeFn wNode, DiscFn disc,
                            std::size_t passes) {
  RelaxState st;
  const auto ids = g.classIds();

  std::deque<Id> worklist;
  std::unordered_set<std::uint32_t> queued;
  for (Id c : ids) {
    worklist.push_back(c);
    queued.insert(g.find(c).value());
  }

  // Generous safety cap on total relaxations so a pathological input cannot spin
  // forever; under the auto pass count this is never reached, so output is
  // unchanged. passes==0 would mean "no bound" but the caller never passes 0.
  const std::size_t nclasses = ids.size() + 1;
  const std::size_t cap =
      passes == 0
          ? std::numeric_limits<std::size_t>::max()
          : (passes > std::numeric_limits<std::size_t>::max() / nclasses
                 ? std::numeric_limits<std::size_t>::max()
                 : passes * nclasses);
  std::size_t steps = 0;

  while (!worklist.empty()) {
    if (++steps > cap) {
      break;
    }
    Id c0 = worklist.front();
    worklist.pop_front();
    const std::uint32_t cid = g.find(c0).value();
    queued.erase(cid);

    const auto &cls = g[c0];
    bool improved = false;
    for (const PathLang &n : cls.nodes) {
      double cost = wNode(n);
      bool eligible = true;
      for (Id q : n.children()) {
        const std::uint32_t qid = g.find(q).value();
        const double cc = st.at(qid);
        if (cc == kInf) {
          eligible = false;
          break;
        }
        cost += cc * disc(qid);
      }
      if (eligible && cost < st.at(cid)) {
        st.cost[cid] = cost;
        st.chosen[cid] = n;
        improved = true;
      }
    }

    if (improved) {
      // This class's cost improved (or became finite): re-relax every class that
      // references it as a child.
      for (Id p : cls.parents) {
        const std::uint32_t pc = g.find(p).value();
        if (queued.insert(pc).second) {
          worklist.push_back(g.find(p));
        }
      }
    }
  }
  return st;
}

// Reference count of each selected class in the DAG rooted at `roots` (roots
// count as one external reference each).
inline std::unordered_map<std::uint32_t, std::size_t>
referenceCounts(Graph &g, const std::vector<Id> &roots,
                const Selection &chosen) {
  std::unordered_map<std::uint32_t, std::size_t> freq;
  std::unordered_set<std::uint32_t> visited;
  std::vector<std::uint32_t> stack;
  for (Id r : roots) {
    const std::uint32_t rid = g.find(r).value();
    ++freq[rid];
    stack.push_back(rid);
  }
  while (!stack.empty()) {
    const std::uint32_t c = stack.back();
    stack.pop_back();
    if (!visited.insert(c).second) {
      continue;
    }
    auto it = chosen.find(c);
    if (it == chosen.end()) {
      continue;
    }
    for (Id q : it->second.children()) {
      const std::uint32_t qid = g.find(q).value();
      ++freq[qid];
      stack.push_back(qid);
    }
  }
  return freq;
}

// Eq. 5: alpha*N_unique + beta*E_unique + sum_o w_o*N_o + gamma*C_repeat.
inline double dagCost(Graph &g, const std::vector<Id> &roots,
                      const Selection &chosen, const CostModel &cm) {
  PathCostFn wf(cm);
  std::unordered_set<std::uint32_t> visited;
  std::vector<std::uint32_t> post; // post-order (children before parents)

  std::function<void(std::uint32_t)> dfs = [&](std::uint32_t c) {
    if (!visited.insert(c).second) {
      return;
    }
    auto it = chosen.find(c);
    if (it != chosen.end()) {
      for (Id q : it->second.children()) {
        dfs(g.find(q).value());
      }
    }
    post.push_back(c);
  };
  for (Id r : roots) {
    dfs(g.find(r).value());
  }

  std::size_t nUnique = post.size();
  std::size_t eUnique = 0;
  double opTerm = 0.0; // sum_o w_o * N_o
  for (std::uint32_t c : post) {
    const PathLang &n = chosen.at(c);
    eUnique += n.children().size();
    opTerm += wf.opWeight(n);
  }

  // Occurrence multiplicity via top-down path counts (reverse post-order is a
  // topological order: parents before children).
  std::unordered_map<std::uint32_t, double> mult;
  for (Id r : roots) {
    mult[g.find(r).value()] += 1.0;
  }
  for (auto it = post.rbegin(); it != post.rend(); ++it) {
    const std::uint32_t c = *it;
    const double mc = mult.count(c) ? mult[c] : 0.0;
    for (Id q : chosen.at(c).children()) {
      mult[g.find(q).value()] += mc;
    }
  }
  double cRepeat = 0.0;
  for (std::uint32_t c : post) {
    const double mc = mult.count(c) ? mult[c] : 0.0;
    cRepeat += (mc > 1.0 ? mc - 1.0 : 0.0) * wf.opWeight(chosen.at(c));
  }

  return cm.alpha * static_cast<double>(nUnique) +
         cm.beta * static_cast<double>(eUnique) + opTerm + cm.gamma * cRepeat;
}

inline bool sameSelection(const Selection &a, const Selection &b) {
  if (a.size() != b.size()) {
    return false;
  }
  for (const auto &kv : a) {
    auto it = b.find(kv.first);
    if (it == b.end() || !(it->second == kv.second)) {
      return false;
    }
  }
  return true;
}

} // namespace detail

// Extract a reuse-aware batch selection from the (clean) e-graph.
inline ExtractResult reuseAwareExtract(Graph &g, const std::vector<Id> &roots,
                                       const CostModel &cm,
                                       const ExtractOptions &opts) {
  PathCostFn wf(cm);
  auto wNode = [&](const PathLang &n) { return wf.opWeight(n); };
  const std::size_t passes =
      opts.relaxPasses ? opts.relaxPasses : g.numberOfClasses() + 1;

  auto noDisc = [](std::uint32_t) { return 1.0; };
  detail::RelaxState best = detail::cycleSafeExtract(g, wNode, noDisc, passes);
  double bestCost = detail::dagCost(g, roots, best.chosen, cm);

  for (std::size_t k = 0; k < opts.reuseIters; ++k) {
    const auto freq = detail::referenceCounts(g, roots, best.chosen);
    auto disc = [&](std::uint32_t qid) {
      auto it = freq.find(qid);
      const double f = (it == freq.end()) ? 0.0 : static_cast<double>(it->second);
      const double extra = f > 1.0 ? f - 1.0 : 0.0;
      return 1.0 / (1.0 + opts.discountLambda * extra);
    };
    detail::RelaxState cand = detail::cycleSafeExtract(g, wNode, disc, passes);
    const double candCost = detail::dagCost(g, roots, cand.chosen, cm);
    const bool unchanged = detail::sameSelection(cand.chosen, best.chosen);
    if (candCost < bestCost) {
      best = std::move(cand);
      bestCost = candCost;
    }
    if (unchanged) {
      break;
    }
  }

  return ExtractResult{std::move(best.chosen), bestCost};
}

// Scalar Eq. 5 cost of the reuse-aware best selection (plateau signal in DAG
// mode).
inline double reuseAwareCost(Graph &g, const std::vector<Id> &roots,
                             const CostModel &cm, const ExtractOptions &opts) {
  return reuseAwareExtract(g, roots, cm, opts).dagCost;
}

} // namespace ean
} // namespace elimination

