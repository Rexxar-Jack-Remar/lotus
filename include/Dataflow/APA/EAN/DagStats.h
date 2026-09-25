#pragma once

// Structural statistics of a hash-consed path-expression DAG (a batch of
// PathExprFactory roots), for the EAN evaluation (paper Table VI). All measures
// are computed over the set of nodes reachable from the given roots.
//
//   uniqueNodes  – distinct hash-consed nodes (retained memory proxy)
//   uniqueEdges  – child links summed over unique nodes (factory construction)
//   unions/concats/stars/atoms/zeros/ones – unique-node counts by kind
//   expandedTree – occurrence multiplicity summed over roots (tree size)
//   maxDepth     – longest root-to-leaf path
//   sharing()    – expandedTree / uniqueNodes (how much hash-consing folds)

#include <algorithm>
#include <cstddef>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Dataflow/APA/Core/PathExpr.h"

namespace elimination {
namespace ean {

struct DagStats {
  std::size_t uniqueNodes = 0;
  std::size_t uniqueEdges = 0;
  std::size_t unions = 0;
  std::size_t concats = 0;
  std::size_t stars = 0;
  std::size_t atoms = 0;
  std::size_t zeros = 0;
  std::size_t ones = 0;
  double expandedTree = 0.0;
  std::size_t maxDepth = 0;

  double sharing() const {
    return uniqueNodes ? expandedTree / static_cast<double>(uniqueNodes) : 0.0;
  }
};

template <typename TransferT>
DagStats computeDagStats(
    const std::vector<typename PathExprFactory<TransferT>::Ref> &roots) {
  using Factory = PathExprFactory<TransferT>;
  using Expr = typename Factory::Expr;
  using Kind = typename Factory::Kind;

  DagStats st;
  std::unordered_set<const Expr *> seen;
  std::vector<const Expr *> post; // post-order (children before parents)
  std::unordered_map<const Expr *, std::size_t> depth;

  std::function<void(const Expr *)> dfs = [&](const Expr *e) {
    if (!e || !seen.insert(e).second) {
      return;
    }
    std::size_t childEdges = 0;
    std::size_t childDepth = 0;
    auto visit = [&](const Expr *c) {
      if (c) {
        ++childEdges;
        dfs(c);
        childDepth = std::max(childDepth, depth[c]);
      }
    };
    switch (e->K) {
    case Kind::Zero: ++st.zeros; break;
    case Kind::One: ++st.ones; break;
    case Kind::Atom: ++st.atoms; break;
    case Kind::Union: ++st.unions; visit(e->L.get()); visit(e->R.get()); break;
    case Kind::Concat: ++st.concats; visit(e->L.get()); visit(e->R.get()); break;
    case Kind::Star: ++st.stars; visit(e->L.get()); break;
    }
    st.uniqueEdges += childEdges;
    depth[e] = 1 + childDepth;
    post.push_back(e);
  };
  for (const auto &r : roots) {
    dfs(r.get());
  }

  st.uniqueNodes = post.size();
  for (const auto &r : roots) {
    if (r) {
      st.maxDepth = std::max(st.maxDepth, depth[r.get()]);
    }
  }

  // Expanded tree size = sum of occurrence multiplicities (top-down path counts;
  // reverse post-order = parents before children).
  std::unordered_map<const Expr *, double> mult;
  for (const auto &r : roots) {
    if (r) {
      mult[r.get()] += 1.0;
    }
  }
  for (auto it = post.rbegin(); it != post.rend(); ++it) {
    const Expr *e = *it;
    const double m = mult.count(e) ? mult[e] : 0.0;
    auto push = [&](const Expr *c) {
      if (c) {
        mult[c] += m;
      }
    };
    switch (e->K) {
    case Kind::Union:
    case Kind::Concat:
      push(e->L.get());
      push(e->R.get());
      break;
    case Kind::Star:
      push(e->L.get());
      break;
    default:
      break;
    }
  }
  for (const Expr *e : post) {
    st.expandedTree += mult.count(e) ? mult[e] : 0.0;
  }

  return st;
}

} // namespace ean
} // namespace elimination

