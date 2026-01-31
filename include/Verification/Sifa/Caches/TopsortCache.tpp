//===-- Verification/Sifa/Caches/TopsortCache.tpp -------------------------===//
//
// Template implementation for TopsortCache.
//
//===----------------------------------------------------------------------===//

#ifndef LOTUS_VERIFICATION_SIFA_CACHES_TOPSORTCACHE_TPP
#define LOTUS_VERIFICATION_SIFA_CACHES_TOPSORTCACHE_TPP

#include "Verification/Sifa/Caches/TopsortCache.h"

#include <queue>
#include <unordered_set>

namespace lotus {
namespace sifa {

template <typename L>
std::vector<typename TopsortCache<L>::Node *> TopsortCache<L>::topsort(const Dag &dag) {
  const auto it = cache_.find(&dag);
  if (it != cache_.end()) {
    return it->second;
  }
  auto order = compute(dag);
  cache_.emplace(&dag, order);
  return order;
}

template <typename L>
std::vector<typename TopsortCache<L>::Node *> TopsortCache<L>::compute(const Dag &dag) {
  using Node = typename TopsortCache<L>::Node;
  // Work on reachable subgraph from source.
  std::unordered_set<Node *> nodes;
  std::queue<Node *> q;
  if (dag.getSource()) {
    q.push(dag.getSource());
    nodes.insert(dag.getSource());
  }
  while (!q.empty()) {
    Node *cur = q.front();
    q.pop();
    for (Node *n : cur->getOutgoingNodes()) {
      if (nodes.insert(n).second) {
        q.push(n);
      }
    }
  }

  std::unordered_map<Node *, int> indeg;
  for (Node *n : nodes) {
    indeg[n] = 0;
  }
  for (Node *n : nodes) {
    for (Node *m : n->getOutgoingNodes()) {
      if (nodes.count(m)) {
        indeg[m] += 1;
      }
    }
  }

  std::queue<Node *> zeros;
  for (auto &kv : indeg) {
    if (kv.second == 0)
      zeros.push(kv.first);
  }

  std::vector<Node *> out;
  while (!zeros.empty()) {
    Node *n = zeros.front();
    zeros.pop();
    out.push_back(n);
    for (Node *m : n->getOutgoingNodes()) {
      if (!nodes.count(m))
        continue;
      int &next = indeg[m];
      next -= 1;
      if (next == 0)
        zeros.push(m);
    }
  }
  return out;
}

} // namespace sifa
} // namespace lotus

#endif // LOTUS_VERIFICATION_SIFA_CACHES_TOPSORTCACHE_TPP
