//
// Created by ahmed on 3/9/2023.
//

#ifndef CPP_CODE_SCC_H
#define CPP_CODE_SCC_H

#include "Dataflow/DemandAPA/Support.h"

// builds 2D vector where each vector is nodes in same SCC
// nodeCC which is the CC ID of each node
// assuming nodes are numbered from 0 to n-1
// CCID corresponds to topo. sorting of SCC

struct SCC {
  vi vis, order, comp, CCID;
  vvi CC, adj, adjR;
  int n;
  SCC(int _n, vii edgeList) : n(_n) {
    vis.assign(n, 0);

    adj.assign(n, vi());
    adjR.assign(n, vi());

    for (auto &pr : edgeList) {
      int u = pr.fs, v = pr.sc;
      adj[u].pb(v);
      adjR[v].pb(u);
    }

    for (int i = 0; i < n; ++i) // change if 0-based
      if (!vis[i])
        dfs1(i);
    vis.assign(n, 0);
    CCID.assign(n, 0);
    reverse(order.begin(), order.end());
    for (auto &u : order)
      if (!vis[u]) {
        comp.clear();
        dfs2(u);
        for (auto &u : comp)
          CCID[u] = sz(CC);
        CC.pb(comp);
      }
  }
  void dfs1(int u) {
    vis[u] = true;
    for (auto &v : adj[u])
      if (!vis[v])
        dfs1(v);
    order.pb(u);
  }
  void dfs2(int u) {
    vis[u] = true;
    comp.pb(u);
    for (auto &v : adjR[u])
      if (!vis[v])
        dfs2(v);
  }
};

#endif // CPP_CODE_SCC_H
