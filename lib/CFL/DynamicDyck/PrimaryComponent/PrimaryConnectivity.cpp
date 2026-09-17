// SPDX-License-Identifier: MIT
#include "CFL/DynamicDyck/PrimaryComponent/PrimaryConnectivity.h"

#include <algorithm>
#include <cassert>
#include <functional>
#include <limits>
#include <stdexcept>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace lotus::cfl::dynamic_dyck::primary_component {
namespace {
using Node = PrimaryConnectivity::Node;
constexpr Node NONE = std::numeric_limits<Node>::max();

struct Pair {
  Node a, b;
  Pair(Node first, Node second)
      : a(std::min(first, second)), b(std::max(first, second)) {}
  bool operator==(const Pair &other) const {
    return a == other.a && b == other.b;
  }
};
struct PairHash {
  std::size_t operator()(const Pair &p) const {
    return std::hash<Node>{}(p.a) ^
           (std::hash<Node>{}(p.b) + std::size_t(0x9e3779b9U) + (p.a << 6) +
            (p.a >> 2));
  }
};

// One permanent (v,v) token per vertex; two directed arc tokens per forest
// edge. Only permanent tokens carry incident-edge marks. Parent pointers allow
// a stable token handle to support rank, root, split and cyclic reroot
// operations.
struct TourNode {
  TourNode *left = nullptr, *right = nullptr, *parent = nullptr;
  std::uint64_t priority;
  Node vertex;
  std::size_t length = 1, vertices, tree_marks = 0, non_tree_marks = 0;
  bool tree_mark = false, non_tree_mark = false;
  TourNode(Node v, std::uint64_t p)
      : priority(p), vertex(v), vertices(v != NONE ? 1 : 0) {}
};
struct ArcPair {
  std::unique_ptr<TourNode> forward, reverse;
};
struct HEdge {
  Pair ends;
  std::uint64_t count = 1;
  std::size_t level = 0;
  bool tree = false;
  std::vector<ArcPair> arcs;
  HEdge(Node a, Node b) : ends(a, b) {}
};
struct VertexData {
  TourNode self;
  // Incidence is stored ONLY at the edge's exact HDT level.
  std::unordered_set<HEdge *> tree_edges, non_tree_edges;
  VertexData(Node v, std::uint64_t p) : self(v, p) {}
};

class Forest {
public:
  explicit Forest(std::size_t level)
      : m_level(level), m_random(0x6a09e667f3bcc909ULL ^ std::uint64_t(level)) {
  }

  void addVertex() {
    const Node v = m_vertices.size();
    m_vertices.push_back(std::make_unique<VertexData>(v, random()));
  }
  TourNode *token(Node v) const { return &m_vertices.at(v)->self; }
  static TourNode *root(TourNode *t) {
    while (t->parent)
      t = t->parent;
    return t;
  }
  bool connected(Node a, Node b) const {
    return root(token(a)) == root(token(b));
  }
  std::size_t componentSize(Node v) const { return root(token(v))->vertices; }

  std::vector<Node> componentVertices(Node v) const {
    std::vector<Node> result;
    result.reserve(componentSize(v));
    std::vector<TourNode *> pending{root(token(v))};
    while (!pending.empty()) {
      TourNode *t = pending.back();
      pending.pop_back();
      if (t->vertex != NONE)
        result.push_back(t->vertex);
      if (t->left)
        pending.push_back(t->left);
      if (t->right)
        pending.push_back(t->right);
    }
    return result;
  }

  void addIncident(HEdge &e, bool tree) {
    for (Node v : {e.ends.a, e.ends.b}) {
      auto &data = *m_vertices[v];
      (tree ? data.tree_edges : data.non_tree_edges).insert(&e);
      refresh(v);
    }
  }
  void removeIncident(HEdge &e, bool tree) {
    for (Node v : {e.ends.a, e.ends.b}) {
      auto &data = *m_vertices[v];
      const auto erased =
          (tree ? data.tree_edges : data.non_tree_edges).erase(&e);
      if (erased != 1)
        throw std::logic_error("HDT incident edge missing");
      refresh(v);
    }
  }

  // The aggregate marks avoid walking an entire component, or its adjacency,
  // to find the next exact-level edge requiring promotion/replacement testing.
  HEdge *firstIncident(Node anchor, bool tree) const {
    TourNode *t = root(token(anchor));
    auto marks = [tree](TourNode *n) {
      return n ? (tree ? n->tree_marks : n->non_tree_marks) : std::size_t(0);
    };
    if (marks(t) == 0)
      return nullptr;
    while (t) {
      if (marks(t->left) != 0) {
        t = t->left;
      } else if (tree ? t->tree_mark : t->non_tree_mark) {
        const auto &data = *m_vertices[t->vertex];
        const auto &edges = tree ? data.tree_edges : data.non_tree_edges;
        return *edges.begin();
      } else {
        t = t->right;
      }
    }
    throw std::logic_error("HDT mark without incident edge");
  }

  void link(HEdge &e) {
    if (connected(e.ends.a, e.ends.b))
      throw std::logic_error("Euler-tour link would create a cycle");
    if (e.arcs.size() <= m_level)
      e.arcs.resize(m_level + 1);
    ArcPair &arcs = e.arcs[m_level];
    if (arcs.forward || arcs.reverse)
      throw std::logic_error("Euler-tour edge already linked");
    arcs.forward = std::make_unique<TourNode>(NONE, random());
    arcs.reverse = std::make_unique<TourNode>(NONE, random());
    TourNode *a = reroot(token(e.ends.a));
    TourNode *b = reroot(token(e.ends.b));
    join(join(join(a, arcs.forward.get()), b), arcs.reverse.get());
  }

  void cut(HEdge &e) {
    if (e.arcs.size() <= m_level || !e.arcs[m_level].forward)
      throw std::logic_error("Euler-tour edge not linked");
    ArcPair &arcs = e.arcs[m_level];
    TourNode *a = arcs.forward.get(), *b = arcs.reverse.get();
    std::size_t first = rank(a), second = rank(b);
    if (first > second) {
      std::swap(first, second);
      std::swap(a, b);
    }
    auto prefix_rest = split(root(a), first);
    auto arc_rest = split(prefix_rest.second, 1);
    auto middle_rest = split(arc_rest.second, second - first - 1);
    auto arc_suffix = split(middle_rest.second, 1);
    // The middle interval is one tree; cyclic prefix+suffix is the other.
    join(prefix_rest.first, arc_suffix.second);
    assert(arc_rest.first == a && arc_suffix.first == b);
    assert(middle_rest.first && middle_rest.first->vertices != 0);
    arcs.forward.reset();
    arcs.reverse.reset();
  }

  const VertexData &vertexData(Node v) const { return *m_vertices.at(v); }

  void validate(std::size_t expected_arcs) const {
    std::unordered_set<TourNode *> roots, seen;
    for (const auto &v : m_vertices)
      roots.insert(root(&v->self));
    using Aggregate =
        std::tuple<std::size_t, std::size_t, std::size_t, std::size_t>;
    std::function<Aggregate(TourNode *, TourNode *)> visit =
        [&](TourNode *t, TourNode *parent) -> Aggregate {
      if (!t)
        return {0, 0, 0, 0};
      if (!seen.insert(t).second || t->parent != parent)
        throw std::logic_error("invalid Euler-tour parent/ownership");
      if (parent && t->priority < parent->priority)
        throw std::logic_error("invalid Euler-tour heap order");
      if (t->vertex != NONE) {
        const auto &v = *m_vertices.at(t->vertex);
        if (&v.self != t || t->tree_mark != !v.tree_edges.empty() ||
            t->non_tree_mark != !v.non_tree_edges.empty())
          throw std::logic_error("invalid Euler-tour vertex marker");
      } else if (t->tree_mark || t->non_tree_mark) {
        throw std::logic_error("arc carries vertex marks");
      }
      const auto l = visit(t->left, t), r = visit(t->right, t);
      Aggregate a{1 + std::get<0>(l) + std::get<0>(r),
                  (t->vertex != NONE ? 1 : 0) + std::get<1>(l) + std::get<1>(r),
                  std::size_t(t->tree_mark) + std::get<2>(l) + std::get<2>(r),
                  std::size_t(t->non_tree_mark) + std::get<3>(l) +
                      std::get<3>(r)};
      if (a !=
          Aggregate{t->length, t->vertices, t->tree_marks, t->non_tree_marks})
        throw std::logic_error("invalid Euler-tour aggregate");
      return a;
    };
    for (TourNode *r : roots)
      visit(r, nullptr);
    if (seen.size() != m_vertices.size() + expected_arcs)
      throw std::logic_error("Euler-tour token count mismatch");
  }

private:
  std::size_t m_level;
  std::uint64_t m_random;
  std::vector<std::unique_ptr<VertexData>> m_vertices;

  std::uint64_t random() {
    // SplitMix64: reproducible per-instance priorities, never global RNG state.
    std::uint64_t z = (m_random += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
  }
  static std::size_t length(TourNode *t) { return t ? t->length : 0; }
  static void pull(TourNode *t) {
    t->length = 1;
    t->vertices = t->vertex != NONE ? 1 : 0;
    t->tree_marks = t->tree_mark ? 1 : 0;
    t->non_tree_marks = t->non_tree_mark ? 1 : 0;
    for (TourNode *child : {t->left, t->right}) {
      if (!child)
        continue;
      t->length += child->length;
      t->vertices += child->vertices;
      t->tree_marks += child->tree_marks;
      t->non_tree_marks += child->non_tree_marks;
    }
  }
  void refresh(Node v) {
    VertexData &data = *m_vertices[v];
    data.self.tree_mark = !data.tree_edges.empty();
    data.self.non_tree_mark = !data.non_tree_edges.empty();
    for (TourNode *t = &data.self; t; t = t->parent)
      pull(t);
  }
  static TourNode *join(TourNode *a, TourNode *b) {
    if (!a || !b) {
      TourNode *t = a ? a : b;
      if (t)
        t->parent = nullptr;
      return t;
    }
    if (a->priority <= b->priority) {
      a->right = join(a->right, b);
      a->right->parent = a;
      pull(a);
      a->parent = nullptr;
      return a;
    }
    b->left = join(a, b->left);
    b->left->parent = b;
    pull(b);
    b->parent = nullptr;
    return b;
  }
  static std::pair<TourNode *, TourNode *> split(TourNode *t, std::size_t k) {
    if (!t) {
      assert(k == 0);
      return {nullptr, nullptr};
    }
    assert(k <= length(t));
    if (k <= length(t->left)) {
      auto result = split(t->left, k);
      t->left = result.second;
      if (t->left)
        t->left->parent = t;
      t->parent = nullptr;
      pull(t);
      return {result.first, t};
    }
    auto result = split(t->right, k - length(t->left) - 1);
    t->right = result.first;
    if (t->right)
      t->right->parent = t;
    t->parent = nullptr;
    pull(t);
    return {t, result.second};
  }
  static std::size_t rank(TourNode *t) {
    std::size_t result = length(t->left);
    while (t->parent) {
      if (t == t->parent->right)
        result += 1 + length(t->parent->left);
      t = t->parent;
    }
    return result;
  }
  static TourNode *reroot(TourNode *t) {
    const auto parts = split(root(t), rank(t));
    return join(parts.second, parts.first);
  }
};
} // namespace

struct PrimaryConnectivity::Impl {
  std::unordered_map<Pair, std::unique_ptr<HEdge>, PairHash> edges;
  std::vector<std::unique_ptr<Forest>> forests;
  // Handles are not vertex IDs. Representatives ARE member vertex IDs. Keeping
  // this distinction lets the larger half retain its handle even when a split
  // separates it from the vertex formerly used as its representative.
  std::vector<Node> handles, representatives, free_handles;
  Statistics stats;

  Impl() { forests.push_back(std::make_unique<Forest>(0)); }
  void check(Node v) const {
    if (v >= handles.size())
      throw std::out_of_range("unknown primary-connectivity vertex");
  }
  Node allocateHandle(Node representative) {
    if (free_handles.empty()) {
      representatives.push_back(representative);
      return representatives.size() - 1;
    }
    const Node h = free_handles.back();
    free_handles.pop_back();
    representatives[h] = representative;
    return h;
  }
  Forest &forest(std::size_t level) {
    while (forests.size() <= level) {
      auto f = std::make_unique<Forest>(forests.size());
      for (std::size_t i = 0; i < handles.size(); ++i)
        f->addVertex();
      forests.push_back(std::move(f));
    }
    return *forests[level];
  }
  void mergeHandles(Node a, Node b) {
    Forest &f = *forests[0];
    if (f.componentSize(a) > f.componentSize(b))
      std::swap(a, b);
    const Node old = handles[a], keep = handles[b];
    for (Node v : f.componentVertices(a)) {
      handles[v] = keep;
      ++stats.relabeled_vertices;
    }
    representatives[old] = NONE;
    free_handles.push_back(old);
    --stats.components;
  }
  void splitHandles(Node a, Node b) {
    Forest &f = *forests[0];
    if (f.componentSize(a) > f.componentSize(b))
      std::swap(a, b);
    const Node keep = handles[b], fresh = allocateHandle(a);
    representatives[keep] = b;
    for (Node v : f.componentVertices(a)) {
      handles[v] = fresh;
      ++stats.relabeled_vertices;
    }
    ++stats.components;
  }
  void link(HEdge &e, std::size_t level) {
    forest(level).link(e);
    ++stats.tree_links;
  }

  // HDT's replacement search. F_i spans edges of level >= i. A component of
  // F_i has at most n/2^i vertices. Moving the smaller cut component's exact-i
  // edges to i+1 preserves that invariant and pays for unsuccessful searches.
  bool replace(Node a, Node b, std::size_t highest) {
    for (std::size_t cursor = highest + 1; cursor != 0; --cursor) {
      const std::size_t i = cursor - 1;
      Forest &f = *forests[i];
      const Node small = f.componentSize(a) <= f.componentSize(b) ? a : b;
      while (HEdge *e = f.firstIncident(small, true)) {
        f.removeIncident(*e, true);
        ++e->level;
        forest(i + 1).addIncident(*e, true);
        link(*e, i + 1);
        ++stats.promotions;
      }
      while (HEdge *e = f.firstIncident(small, false)) {
        ++stats.scanned_non_tree;
        f.removeIncident(*e, false);
        if (f.connected(e->ends.a, e->ends.b)) {
          ++e->level;
          forest(i + 1).addIncident(*e, false);
          ++stats.promotions;
        } else {
          e->tree = true;
          f.addIncident(*e, true);
          for (std::size_t j = 0; j <= i; ++j)
            link(*e, j);
          ++stats.replacements;
          return true;
        }
      }
    }
    return false;
  }
};

PrimaryConnectivity::PrimaryConnectivity() : m_impl(std::make_unique<Impl>()) {}
PrimaryConnectivity::~PrimaryConnectivity() = default;
PrimaryConnectivity::PrimaryConnectivity(PrimaryConnectivity &&) noexcept =
    default;
PrimaryConnectivity &
PrimaryConnectivity::operator=(PrimaryConnectivity &&) noexcept = default;

PrimaryConnectivity::Node PrimaryConnectivity::addVertex() {
  const Node result = m_impl->handles.size();
  if (result == NONE)
    throw std::length_error("too many primary-connectivity vertices");
  m_impl->handles.push_back(m_impl->allocateHandle(result));
  for (auto &f : m_impl->forests)
    f->addVertex();
  ++m_impl->stats.components;
  return result;
}
void PrimaryConnectivity::insertEdge(Node first, Node second) {
  Impl &d = *m_impl;
  d.check(first);
  d.check(second);
  auto found = d.edges.find(Pair(first, second));
  if (found != d.edges.end()) {
    if (found->second->count == std::numeric_limits<std::uint64_t>::max())
      throw std::overflow_error("primal support count overflow");
    ++found->second->count;
    ++d.stats.insertions;
    return;
  }
  auto edge = std::make_unique<HEdge>(first, second);
  HEdge &e = *edge;
  d.edges.emplace(e.ends, std::move(edge));
  if (first != second) {
    if (d.handles[first] != d.handles[second]) {
      e.tree = true;
      d.mergeHandles(first, second);
      d.forests[0]->addIncident(e, true);
      d.link(e, 0);
    } else {
      d.forests[0]->addIncident(e, false);
    }
  }
  ++d.stats.insertions;
}
bool PrimaryConnectivity::deleteEdge(Node first, Node second) {
  Impl &d = *m_impl;
  d.check(first);
  d.check(second);
  const auto found = d.edges.find(Pair(first, second));
  if (found == d.edges.end())
    return false;
  HEdge &e = *found->second;
  ++d.stats.deletions;
  if (--e.count != 0)
    return true;
  if (first != second) {
    d.forests[e.level]->removeIncident(e, e.tree);
    if (e.tree) {
      for (std::size_t i = 0; i <= e.level; ++i) {
        d.forests[i]->cut(e);
        ++d.stats.tree_cuts;
      }
      if (!d.replace(first, second, e.level))
        d.splitHandles(first, second);
    }
  }
  d.edges.erase(found);
  return true;
}
PrimaryConnectivity::Node PrimaryConnectivity::representative(Node node) const {
  m_impl->check(node);
  return m_impl->representatives[m_impl->handles[node]];
}
bool PrimaryConnectivity::connected(Node first, Node second) const {
  m_impl->check(first);
  m_impl->check(second);
  return m_impl->handles[first] == m_impl->handles[second];
}
PrimaryConnectivity::Statistics PrimaryConnectivity::statistics() const {
  Statistics result = m_impl->stats;
  result.vertices = m_impl->handles.size();
  result.edges = m_impl->edges.size();
  result.levels = m_impl->forests.size();
  return result;
}
std::vector<PrimaryConnectivity::Support>
PrimaryConnectivity::supports() const {
  std::vector<Support> result;
  result.reserve(m_impl->edges.size());
  for (const auto &entry : m_impl->edges)
    result.push_back({entry.first.a, entry.first.b, entry.second->count});
  std::sort(result.begin(), result.end(),
            [](const Support &a, const Support &b) {
              return std::tie(a.first, a.second) < std::tie(b.first, b.second);
            });
  return result;
}
bool PrimaryConnectivity::validate(std::string *error) const {
  try {
    const Impl &d = *m_impl;
    const std::size_t n = d.handles.size();
    std::unordered_set<Node> active;
    for (Node v = 0; v < n; ++v) {
      const Node h = d.handles[v];
      if (h >= d.representatives.size() || d.representatives[h] >= n ||
          d.handles[d.representatives[h]] != h)
        throw std::logic_error("invalid primary component handle");
      active.insert(h);
    }
    if (active.size() != d.stats.components ||
        active.size() + d.free_handles.size() != d.representatives.size())
      throw std::logic_error("primary component count mismatch");
    std::unordered_set<Node> free;
    for (Node h : d.free_handles)
      if (active.count(h) || !free.insert(h).second ||
          d.representatives.at(h) != NONE)
        throw std::logic_error("invalid free primary handle");

    for (std::size_t i = 0; i < d.forests.size(); ++i) {
      const Forest &f = *d.forests[i];
      std::size_t forest_edges = 0;
      std::vector<std::vector<Node>> adjacency(n);
      for (const auto &entry : d.edges) {
        const HEdge &e = *entry.second;
        if (e.count == 0 || e.level >= d.forests.size())
          throw std::logic_error("invalid HDT edge count/level");
        if (e.ends.a == e.ends.b)
          continue;
        if (e.level >= i) {
          adjacency[e.ends.a].push_back(e.ends.b);
          adjacency[e.ends.b].push_back(e.ends.a);
          if (e.tree) {
            ++forest_edges;
            if (e.arcs.size() <= i || !e.arcs[i].forward ||
                !e.arcs[i].reverse ||
                Forest::root(e.arcs[i].forward.get()) !=
                    Forest::root(f.token(e.ends.a)) ||
                Forest::root(e.arcs[i].reverse.get()) !=
                    Forest::root(f.token(e.ends.b)))
              throw std::logic_error("HDT forest membership mismatch");
          }
        }
        for (Node v : {e.ends.a, e.ends.b}) {
          const auto &data = f.vertexData(v);
          if (bool(data.tree_edges.count(entry.second.get())) !=
                  (e.tree && e.level == i) ||
              bool(data.non_tree_edges.count(entry.second.get())) !=
                  (!e.tree && e.level == i))
            throw std::logic_error("HDT exact-level incidence mismatch");
        }
      }
      f.validate(2 * forest_edges);
      std::vector<bool> visited(n, false);
      std::unordered_set<TourNode *> component_roots;
      for (Node start = 0; start < n; ++start) {
        if (visited[start])
          continue;
        TourNode *root = Forest::root(f.token(start));
        if (!component_roots.insert(root).second)
          throw std::logic_error("HDT forest overconnects graph");
        std::vector<Node> stack{start};
        visited[start] = true;
        std::size_t size = 0;
        while (!stack.empty()) {
          const Node v = stack.back();
          stack.pop_back();
          ++size;
          if (Forest::root(f.token(v)) != root ||
              (i == 0 && d.handles[v] != d.handles[start]))
            throw std::logic_error("HDT forest does not span graph");
          for (Node w : adjacency[v])
            if (!visited[w]) {
              visited[w] = true;
              stack.push_back(w);
            }
        }
        if (size != root->vertices ||
            i >= std::numeric_limits<std::size_t>::digits || size > (n >> i))
          throw std::logic_error("HDT component-size invariant violated");
      }
      if (i == 0 && component_roots.size() != active.size())
        throw std::logic_error("HDT handle partition mismatch");
    }
    if (error)
      error->clear();
    return true;
  } catch (const std::logic_error &failure) {
    if (error)
      *error = failure.what();
    return false;
  }
}
} // namespace lotus::cfl::dynamic_dyck::primary_component
