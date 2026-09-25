#ifndef FASTIFDS_LCA_H
#define FASTIFDS_LCA_H
#include "Dataflow/DemandAPA/SparseTable.h"
#include "Dataflow/DemandAPA/Support.h"

/*
 * Computes LCA of a tree. Takes as input an array of length n corresponding to
 * a tree of n nodes labeled with [0, n). Root has par = -1. O(n logn)
 * preprocessing and O(1) queryUisStart time
 */

struct LCA {
  int n, idx;
  vi lvl, seq, fsOcc;
  vvi adj;
  SparseTable RMQ;
  void dfs(int u, int level, int parent) {
    fsOcc[u] = idx;
    seq[idx] = u;
    lvl[idx++] = level;
    for (auto &v : adj[u])
      if (v != parent) {
        dfs(v, level + 1, u);
        seq[idx] = u;
        lvl[idx++] = level;
      }
  }

  LCA() {}

  LCA(vi par) {
    n = sz(par);
    adj.assign(n, vi());
    assert(n > 0);
    int root = -1;
    for (int u = 0; u < n; ++u)
      if (par[u] == -1) {
        assert(root == -1);
        root = u;
      } else
        adj[u].pb(par[u]), adj[par[u]].pb(u);
    assert(root != -1);
    lvl.assign(2 * (n + 1), 0);
    seq.assign(2 * (n + 1), 0);
    fsOcc.assign(n + 1, 0);
    idx = 0;
    dfs(root, 0, -1);
    RMQ = SparseTable(lvl);
  }
  int query(int u, int v) {
    assert(min(u, v) >= 0 && max(u, v) < n);
    if (fsOcc[u] > fsOcc[v])
      swap(u, v);
    return seq[RMQ.query(fsOcc[u], fsOcc[v])];
  }
};

#endif // FASTIFDS_LCA_H
