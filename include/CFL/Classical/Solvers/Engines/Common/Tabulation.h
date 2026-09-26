// SPDX-License-Identifier: MIT
#pragma once
#include "CFL/Classical/Solvers/Engines/Common/Reachability.h"

#include <algorithm>
#include <deque>
#include <limits>
#include <numeric>
#include <unordered_map>
#include <unordered_set>

namespace lotus::cfl::classical::common::detail {
using Vertex = std::uint32_t;
struct Edge {
  Vertex source;
  Symbol symbol;
  Vertex target;
  bool operator==(const Edge &e) const {
    return source == e.source && symbol == e.symbol && target == e.target;
  }
};
inline std::size_t hash64(std::uint64_t x) {
  x ^= x >> 30;
  x *= UINT64_C(0xbf58476d1ce4e5b9);
  x ^= x >> 27;
  x *= UINT64_C(0x94d049bb133111eb);
  return static_cast<std::size_t>(x ^ (x >> 31));
}
struct EdgeHash {
  std::size_t operator()(const Edge &e) const {
    return hash64((std::uint64_t{e.source} << 32) | e.target) ^
           hash64(std::uint64_t{e.symbol} + UINT64_C(0x9e3779b97f4a7c15));
  }
};
inline std::uint64_t key(Symbol s, Vertex v) {
  return (std::uint64_t{s} << 32) | v;
}
struct Row {
  std::unordered_set<Vertex> set;
  std::vector<Vertex> list;
};
// Pointer/reference stability: unordered_map rehash preserves references to
// rows. Copy vertex values and snapshot row.list.size() before insertions.
// No erasure takes place during a tabulation stage.
class Relation {
public:
  const Row *out(Symbol s, Vertex v) const { return row(out_, key(s, v)); }
  const Row *in(Symbol s, Vertex v) const { return row(in_, key(s, v)); }
  bool contains(const Edge &e) const {
    const auto *p = out(e.symbol, e.source);
    if (p && p->set.count(e.target))
      return true;
    p = in(e.symbol, e.target);
    return p && p->set.count(e.source);
  }
  bool insert(const Edge &e, bool outgoing = true, bool incoming = true) {
    bool changed = false;
    if (outgoing)
      changed |= add(out_, key(e.symbol, e.source), e.target);
    if (incoming)
      changed |= add(in_, key(e.symbol, e.target), e.source);
    return changed;
  }
  std::vector<Edge> edges() const {
    std::unordered_set<Edge, EdgeHash> seen;
    for (const auto &p : out_)
      for (Vertex v : p.second.list)
        seen.insert({static_cast<Vertex>(p.first),
                     static_cast<Symbol>(p.first >> 32), v});
    for (const auto &p : in_)
      for (Vertex u : p.second.list)
        seen.insert({u, static_cast<Symbol>(p.first >> 32),
                     static_cast<Vertex>(p.first)});
    return {seen.begin(), seen.end()};
  }
  std::size_t degree() const {
    std::size_t n = 0;
    for (const auto &p : out_)
      n += p.second.list.size();
    for (const auto &p : in_)
      n += p.second.list.size();
    return n;
  }

private:
  using Rows = std::unordered_map<std::uint64_t, Row>;
  static const Row *row(const Rows &m, std::uint64_t k) {
    auto it = m.find(k);
    return it == m.end() ? nullptr : &it->second;
  }
  static bool add(Rows &m, std::uint64_t k, Vertex v) {
    auto &r = m[k];
    if (!r.set.insert(v).second)
      return false;
    r.list.push_back(v);
    return true;
  }
  Rows out_, in_;
};
struct Worklist {
  std::deque<Edge> queue;
  std::unordered_set<Edge, EdgeHash> pending;
  void push(Edge e) {
    if (pending.insert(e).second)
      queue.push_back(e);
  }
  Edge pop() {
    Edge e = queue.front();
    queue.pop_front();
    pending.erase(e);
    return e;
  }
  bool empty() const { return queue.empty(); }
  void clear() {
    queue.clear();
    pending.clear();
  }
};
struct RuleIndex {
  std::vector<Rule> rules;
  std::vector<std::vector<std::size_t>> unary, first, second;
  std::vector<Symbol> epsilon;
  explicit RuleIndex(const Grammar &g)
      : rules(g.rules), unary(g.terminal.size()), first(g.terminal.size()),
        second(g.terminal.size()) {
    for (auto &r : rules) {
      if (r.arity == 0)
        r.first = 0;
      if (r.arity < 2)
        r.second = 0;
    }
    std::sort(rules.begin(), rules.end());
    rules.erase(std::unique(rules.begin(), rules.end()), rules.end());
    for (std::size_t i = 0; i < rules.size(); ++i) {
      const auto &r = rules[i];
      if (r.arity == 0)
        epsilon.push_back(r.lhs);
      else if (r.arity == 1)
        unary[r.first].push_back(i);
      else {
        first[r.first].push_back(i);
        second[r.second].push_back(i);
      }
    }
  }
};
struct Input {
  std::vector<Node> nodes;
  std::vector<Edge> edges;
  std::vector<Symbol> outputs;
  std::vector<bool> selected;
  Input(const Grammar &g, const Graph &graph, const Query &q)
      : selected(g.terminal.size(), false) {
    g.validate();
    nodes = graph.nodes;
    for (const auto &f : graph.edges) {
      if (f.symbol >= g.terminal.size() || !g.terminal[f.symbol])
        throw std::invalid_argument(
            "base graph contains a nonterminal/undeclared edge");
      nodes.push_back(f.source);
      nodes.push_back(f.target);
    }
    std::sort(nodes.begin(), nodes.end());
    nodes.erase(std::unique(nodes.begin(), nodes.end()), nodes.end());
    if (nodes.size() > std::numeric_limits<Vertex>::max())
      throw ResourceLimit("too many vertices");
    if (q.scope == Scope::AllSymbols) {
      if (!q.targets.empty())
        throw std::invalid_argument("AllSymbols cannot specify targets");
      outputs.resize(g.terminal.size());
      std::iota(outputs.begin(), outputs.end(), Symbol{0});
    } else {
      outputs = q.targets.empty() ? std::vector<Symbol>{g.start} : q.targets;
      for (Symbol s : outputs)
        if (s >= g.terminal.size() || g.terminal[s])
          throw std::invalid_argument("target is not a nonterminal");
      std::sort(outputs.begin(), outputs.end());
      outputs.erase(std::unique(outputs.begin(), outputs.end()), outputs.end());
    }
    for (Symbol s : outputs)
      selected[s] = true;
    std::unordered_map<Node, Vertex> ids;
    for (std::size_t i = 0; i < nodes.size(); ++i)
      ids.emplace(nodes[i], static_cast<Vertex>(i));
    std::set<Fact> unique(graph.edges.begin(), graph.edges.end());
    for (const Fact &f : unique)
      edges.push_back({ids.at(f.source), f.symbol, ids.at(f.target)});
  }
};
inline void tick(std::size_t &value, std::size_t limit, const char *what) {
  if (value == std::numeric_limits<std::size_t>::max() ||
      (limit && value >= limit))
    throw ResourceLimit(what);
  ++value;
}
inline std::vector<bool> nullable(const Grammar &g) {
  std::vector<bool> n(g.terminal.size(), false);
  bool change;
  do {
    change = false;
    for (const auto &r : g.rules) {
      bool yes = r.arity == 0 || (r.arity == 1 && n[r.first]) ||
                 (r.arity == 2 && n[r.first] && n[r.second]);
      if (yes && !n[r.lhs]) {
        n[r.lhs] = true;
        change = true;
      }
    }
  } while (change);
  return n;
}
} // namespace lotus::cfl::classical::common::detail
