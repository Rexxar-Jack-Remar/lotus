#pragma once

#include "Dataflow/DemandAPA/DemandOmp.h"
#include "Dataflow/DemandAPA/RegEx.h"
#include "Dataflow/DemandAPA/Support.h"

using namespace std;

class PathSequence {
private:
  RegEx *P;
  int u, v;

public:
  PathSequence(RegEx *_P, int _u, int _v) : P(_P), u(_u), v(_v) {}
  void print() { cout << *P << " " << u << "-" << v << endl; }
  bool equal(int _u, int _v) { return (u == _u and v == _v); }
  RegEx *getP() { return P; }
  int getU() { return u; }
  int getV() { return v; }
};

class DominatorTree {
private:
  int T;
  vector<vector<int>> g, tree, rg, bucket;
  vector<int> sdom, par, dom, dsu, label;
  vector<int> arr, rev, idom, in, out;

  void dfs0(int u) {
    T += 1;
    arr[u] = T;
    rev[T] = u;
    label[T] = sdom[T] = dsu[T] = T;
    for (auto w : g[u]) {
      if (!arr[w])
        dfs0(w), par[arr[w]] = arr[u];
      rg[arr[w]].pb(arr[u]);
    }
  }

  void dfs1(int u) {
    in[u] = ++T;
    for (auto w : tree[u])
      dfs1(w);
    out[u] = T;
  }

  int Find(int u, int x = 0) {
    if (u == dsu[u]) {
      if (x)
        return -1;
      return u;
    }
    int v = Find(dsu[u], x + 1);
    if (v < 0)
      return u;
    if (sdom[label[dsu[u]]] < sdom[label[u]])
      label[u] = label[dsu[u]];
    dsu[u] = v;
    return x ? v : label[u];
  }

  void Union(int u, int v) { dsu[v] = u; }

public:
  DominatorTree() {}
  DominatorTree(int n, vector<pair<int, int>> edges) {
    g.assign(n + 1, vector<int>());      // no need
    tree.assign(n + 1, vector<int>());   // done
    rg.assign(n + 1, vector<int>());     // done
    bucket.assign(n + 1, vector<int>()); // done
    arr.assign(n + 1, 0);                // done
    label.assign(n + 1, 0);              // done
    sdom.assign(n + 1, 0);               // done
    dsu.assign(n + 1, 0);                // done
    idom.assign(n + 1, 0);               // done
    rev.assign(n + 1, 0);                // done
    dom.assign(n + 1, 0);                // done
    par.assign(n + 1, 0);                // done
    in.assign(n + 1, 0);                 // done
    out.assign(n + 1, 0);                // done
    for (auto edge : edges)
      g[edge.ff].pb(edge.ss);
  }

  void build(vector<int> &reach) {
    T = 0;
    int source = reach[0];
    dfs0(source);
    int n = T;
    for (int i = n; i >= 1; i--) {
      for (auto x : rg[i])
        sdom[i] = min(sdom[i], sdom[Find(x)]);
      if (i > 1)
        bucket[sdom[i]].pb(i);
      for (auto w : bucket[i]) {
        int v = Find(w);
        if (sdom[v] == sdom[w])
          dom[w] = sdom[w];
        else
          dom[w] = v;
      }
      if (i > 1)
        Union(par[i], i);
    }
    for (int i = 2; i <= n; i++)
      if (sdom[i] != dom[i])
        dom[i] = dom[dom[i]];
    idom[rev[1]] = 0;
    for (int i = 2; i <= n; i++)
      idom[rev[i]] = rev[dom[i]];

    // clearning
    for (int i = 1; i <= n; i++) {
      rg[i].clear();
      bucket[i].clear();
      label[i] = rev[i] = dom[i] = 0;
      sdom[i] = dsu[i] = arr[i] = par[i] = 0;
    }
    for (auto v : reach)
      if (v != source)
        tree[idom[v]].pb(v);
    T = 0;
    dfs1(source);
    for (auto v : reach) {
      arr[v] = 0;
      tree[v].clear();
    }
  }

  int get_idom(int x) { return idom[x]; }

  bool is_ancestor(int u, int v) {
    return (in[u] <= in[v] and out[v] <= out[u]);
  }
};

class Tarjan {
private:
  int n, m, T, source;

  vector<int> out;
  vector<int> order, decompose_order, ancestor;
  vector<RegEx *> Pt;

  vector<RegEx *> ptrs;

  RegEx *getptr(ll t, RegEx *L = nullptr, RegEx *R = nullptr) {

    RegEx *gen = nullptr;
    if (t == RegEx::ZERO or t == RegEx::ONE or t == RegEx::PROJECT)
      gen = new RegEx(t);
    if (t == RegEx::STAR)
      gen = new RegEx(t, L, nullptr);
    if (t == RegEx::PLUS or t == RegEx::DOT)
      gen = new RegEx(t, L, R);
    if (t >= 0)
      gen = new RegEx(t);
    ptrs.pb(gen);
    return gen;
  }

  vector<vector<RegEx *>> p;
  vector<RegEx *> S;
  vector<PathSequence> sequence;
  vector<vector<PII>> adj;
  vector<vector<int>> children, tree, nontree, sibling;
  vector<vector<int>> G, G_inv;
  vector<int> vis, h, t, reach;
  vector<int> id, sccOrder, sccId, reach_edges;
  vector<vector<int>> D;
  DominatorTree DT;

  vector<vector<pair<vector<pair<pair<int, int>, int>>,
                     vector<pair<pair<int, int>, int>>>>>
      SC;

public:
  Tarjan(int _n, int _m, vector<pair<int, int>> edges) {
    n = _n;
    m = _m;

    vis.assign(n + 1, 0);
    out.assign(n + 1, 0);
    ancestor.assign(n + 1, 0);
    S.assign(n + 1, NULL);
    sccId.assign(n + 1, 0);
    h.assign(m + 1, 0);
    t.assign(m + 1, 0);
    adj.assign(n + 1, vector<PII>());
    sibling.assign(n + 1, vector<int>());
    // anc.assign(n + 1, vector<int>());
    children.assign(n + 1, vector<int>());
    tree.assign(n + 1, vector<int>());
    nontree.assign(n + 1, vector<int>());
    G.assign(n + 1, vector<int>());
    G_inv.assign(n + 1, vector<int>());
    // idom.assign(n + 1, 0);
    // depth.assign(n + 1, 0);
    id.assign(n + 1, 0);
    reach.clear();
    reach_edges.clear();
    Pt.assign(n + 1, nullptr);
    D.assign(n + 1, vector<int>());
    SC.assign(n + 1, vector<pair<vector<pair<pair<int, int>, int>>,
                                 vector<pair<pair<int, int>, int>>>>());
    DT = DominatorTree(n, edges);
    for (int i = 0; i < m; i++)
      add_edge(edges[i].ff, edges[i].ss, i + 1);
  }

  void clearPointers() {
    sort(ptrs.begin(), ptrs.end());
    int sz = ptrs.size(), j, i = 0;
    while (i < sz) {
      j = i;
      while (ptrs[i] == ptrs[j])
        j += 1;
      delete ptrs[i];
      i = j;
    }
    ptrs.clear();
  }

  void clearing() {
    // Clear structures
    reach.pb(0);
    for (auto v : reach) {
      tree[v].clear();
      nontree[v].clear();
      children[v].clear();
      sibling[v].clear();
      SC[v].clear();
    }

    reach.clear();
    reach_edges.clear();
    sequence.clear();
  }

  RegEx *query(int u, int v) {
    source = u;
    reachable(source);
    bool reachable = vis[v];
    for (auto w : reach)
      vis[w] = 0;
    RegEx *answer = getptr(RegEx::ZERO);
    if (reachable) {
      compute();
      decompose_and_sequence();
      solve();
      // If you want to print answer for all reachable nodes
      // show_answer();
      answer = Pt[v];
    }
    clearing();
    return answer;
  }

  void add_edge(int u, int v, int i) {
    adj[u].pb(mp(v, i));
    h[i] = u;
    t[i] = v;
  }

  // Algorithm starts here
  void solve() {
    Pt[source] = getptr(RegEx::ONE);
    for (auto v : reach)
      if (v != source)
        Pt[v] = getptr(RegEx::ZERO);
    for (auto seq : sequence) {
      int v = seq.getU();
      int w = seq.getV();
      if (v == w)
        Pt[v] = getptr(RegEx::DOT, Pt[v], seq.getP());
      else
        Pt[w] =
            getptr(RegEx::PLUS, Pt[w], getptr(RegEx::DOT, Pt[v], seq.getP()));
    }
  }

  void show_answer() {
    for (auto v : reach) {
      if (v == source)
        cout << "source " << v << ": ";
      else
        cout << "node " << v << ": ";
      if (Pt[v] != NULL)
        cout << *Pt[v] << "\n";
      else
        cout << "\n";
    }
    puts("--------------------------");
  }

  vector<PathSequence> eleminate(vector<pair<PII, RegEx *>> &edges) {
    if (edges.empty())
      return {};
    map<int, int> id, who;
    int n_edges = edges.size(), vertices = 0;
    for (auto edge : edges) {
      int u = edge.ff.ff;
      int v = edge.ff.ss;
      if (id.find(u) == id.end()) {
        id[u] = vertices;
        who[vertices] = u;
        vertices += 1;
      }
      if (id.find(v) == id.end()) {
        id[v] = vertices;
        who[vertices] = v;
        vertices += 1;
      }
    }
    p.assign(vertices, vector<RegEx *>(vertices, NULL));
    for (int v = 0; v < vertices; v++)
      for (int w = 0; w < vertices; w++)
        p[v][w] = getptr(RegEx::ZERO);
    for (auto edge : edges) {
      int u = id[edge.ff.ff];
      int v = id[edge.ff.ss];
      p[u][v] = getptr(RegEx::PLUS, p[u][v], edge.ss);
    }
    for (int v = 0; v < vertices; v++) {
      p[v][v] = getptr(RegEx::STAR, p[v][v], nullptr);
      for (int u = v + 1; u < vertices; u++) {
        if (p[u][v]->eId != RegEx::ZERO) {
          p[u][v] = getptr(RegEx::DOT, p[u][v], p[v][v]);
          for (int w = v + 1; w < vertices; w++)
            if (p[v][w]->eId != RegEx::ZERO) {
              p[u][w] = getptr(RegEx::PLUS, p[u][w],
                               getptr(RegEx::DOT, p[u][v], p[v][w]));
            }
        }
      }
    }
    vector<PathSequence> Y;
    for (int u = 0; u < vertices; u++)
      for (int v = u; v < vertices; v++)
        if (p[u][v]->eId != RegEx::ZERO)
          Y.pb(PathSequence(p[u][v], who[u], who[v]));
    for (int u = vertices - 1; u >= 0; u--)
      for (int v = 0; v < u; v++)
        if (p[u][v]->eId != RegEx::ZERO)
          Y.pb(PathSequence(p[u][v], who[u], who[v]));
    p.clear();
    return Y;
  }

  void init() {
    for (auto v : reach) {
      ancestor[v] = 0;
      S[v] = getptr(RegEx::ONE);
    }
  }

  void update(int w, int v, RegEx *R) {
    ancestor[v] = w;
    S[v] = R;
  }

  pair<RegEx *, int> eval_and_compress(int v, int e) {
    if (ancestor[v] == 0)
      return mp(getptr(RegEx::ONE), v);
    pair<RegEx *, int> parent = eval_and_compress(ancestor[v], e);
    ancestor[v] = parent.ss;
    S[v] = getptr(RegEx::DOT, parent.ff, S[v]);
    sequence.push_back(
        PathSequence(getptr(RegEx::DOT, S[v], get_edge(e)), v, t[e]));
    return mp(S[v], ancestor[v]);
  }

  RegEx *eval_and_sequence(int e) {
    return getptr(RegEx::DOT, eval_and_compress(h[e], e).ff, get_edge(e));
  }

  void reachable(int nd) {
    vis[nd] = 1;
    reach.pb(nd);
    for (auto e : adj[nd]) {
      reach_edges.pb(e.ss);
      if (!vis[e.ff])
        reachable(e.ff);
    }
  }

  void give_id(int nd) {
    id[nd] = T--;
    for (auto to : children[nd])
      give_id(to);
  }

  void dfs0(int nd) {
    vis[nd] = 1;
    for (auto e : adj[nd])
      if (!vis[e.ff])
        dfs0(e.ff);
  }
  // SCC dfs's
  void dfs1(int u) {
    vis[u] = true;
    for (auto e : G[u]) {
      int v = t[e];
      if (!vis[v])
        dfs1(v);
    }
    sccOrder.pb(u);
  }

  void dfs2(int u, vector<int> &comp) {
    vis[u] = true;
    comp.pb(u);
    for (auto v : G_inv[u])
      if (!vis[v])
        dfs2(v, comp);
  }

  RegEx *get_edge(int e) { return getptr((((ll)(h[e])) << 32) | t[e]); }

  void compute() {
    DT.build(reach);
    for (auto v : reach)
      children[DT.get_idom(v)].pb(v);
    for (auto e : reach_edges) {
      int u = t[e];
      if (h[e] == DT.get_idom(u))
        tree[u].pb(e);
      else
        nontree[u].pb(e);
    }
    T = n;
    give_id(0);
    // get derived graph
    for (auto e : reach_edges) {
      if (t[e] == source)
        sibling[source].pb(e);
      else if (h[e] != DT.get_idom(t[e])) {
        for (auto u : children[DT.get_idom(t[e])])
          if (DT.is_ancestor(u, h[e]))
            sibling[u].pb(e);
      }
    }

    // compute SCC for each sibling set
    vector<int> comp;
    vector<vector<int>> SCCL, SCCR;
    queue<int> q;
    vector<pair<pair<int, int>, int>> X, Y;
    reach.pb(0);
    for (auto u : reach) {
      sccOrder.clear();
      SCCL.clear();
      SCCR.clear();
      for (auto v : children[u])
        for (auto e : sibling[v]) {
          G[v].pb(e);
          G_inv[t[e]].pb(v);
        }
      for (auto v : children[u])
        if (!vis[v])
          dfs1(v);
      for (auto v : children[u])
        vis[v] = 0;
      reverse(all(sccOrder));
      for (auto v : sccOrder)
        if (!vis[v]) {
          comp.clear();
          dfs2(v, comp);
          for (auto r : comp)
            sccId[r] = SCCL.size();
          SCCL.pb(comp);
        }
      // Topological sorting on SCC transformed graph
      int nl = SCCL.size();
      for (auto v : children[u])
        for (auto e : sibling[v])
          if (sccId[v] != sccId[t[e]]) {
            D[sccId[t[e]]].pb(sccId[v]);
            out[sccId[v]] += 1;
          }
      for (int i = 0; i < nl; i++)
        if (!out[i])
          q.push(i);
      while (!q.empty()) {
        int nd = q.front();
        q.pop();
        SCCR.pb(SCCL[nd]);
        for (auto to : D[nd])
          if (!(--out[to]))
            q.push(to);
      }
      reverse(all(SCCR));
      for (auto comp : SCCR) {
        X.clear();
        Y.clear();
        for (auto v : comp)
          for (auto e : G[v]) {
            int w = t[e];
            if (sccId[v] == sccId[w])
              X.pb(mp(mp(v, w), e));
            if (sccId[v] != sccId[w])
              Y.pb(mp(mp(v, w), e));
          }
        SC[u].pb(mp(X, Y));
      }
      for (auto v : children[u]) {
        vis[v] = 0;
        G[v].clear();
        G_inv[v].clear();
      }
      for (int i = 0; i < nl; i++) {
        out[i] = 0;
        D[i].clear();
      }
    }
    reach.pop_back();
  }

  void decompose_and_sequence() {
    init();
    vector<pair<PII, RegEx *>> edges;
    vector<RegEx *> P(m + 1), R(n + 1);
    vector<int> decompose_order = reach;
    auto cmp = [&](int x, int y) { return id[x] < id[y]; };
    sort(all(decompose_order), cmp);
    vector<PathSequence> Yu;
    for (auto u : decompose_order) {
      if (children[u].empty())
        continue;
      for (auto v : children[u])
        for (auto e : nontree[v])
          P[e] = eval_and_sequence(e);
      for (auto comp : SC[u]) {
        edges.clear();
        for (auto edge : comp.ff)
          edges.pb(mp(edge.ff, P[edge.ss]));
        for (auto path : eleminate(edges))
          Yu.pb(path);
        for (auto edge : comp.ss)
          Yu.pb(PathSequence(P[edge.ss], edge.ff.ff, edge.ff.ss));
      }
      for (auto path : Yu)
        sequence.pb(path);
      for (auto v : children[u]) {
        R[v] = getptr(RegEx::ZERO);
        for (auto e : tree[v])
          R[v] = getptr(RegEx::PLUS, R[v], get_edge(e));
      }
      for (auto path : Yu) {
        int w = path.getU();
        int x = path.getV();
        if (w == x)
          R[w] = getptr(RegEx::DOT, R[w], path.getP());
        else
          R[x] =
              getptr(RegEx::PLUS, R[x], getptr(RegEx::DOT, R[w], path.getP()));
      }
      for (auto v : children[u])
        update(u, v, R[v]);
      Yu.clear();
    }
    RegEx *Q = getptr(RegEx::ZERO);
    for (auto e : nontree[source])
      Q = getptr(RegEx::PLUS, Q, eval_and_sequence(e));
    if (Q->eId != RegEx::ONE and Q->eId != RegEx::ZERO)
      sequence.pb(
          PathSequence(getptr(RegEx::STAR, Q, nullptr), source, source));
    for (int i = int(decompose_order.size()) - 2; i >= 0; i--) {
      int v = decompose_order[i];
      sequence.pb(PathSequence(S[v], ancestor[v], v));
    }
  }
};

