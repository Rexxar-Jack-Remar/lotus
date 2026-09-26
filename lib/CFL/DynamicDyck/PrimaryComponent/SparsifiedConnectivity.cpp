// SPDX-License-Identifier: MIT
#include "CFL/DynamicDyck/PrimaryComponent/SparsifiedConnectivity.h"

#include <algorithm>
#include <array>
#include <limits>
#include <map>
#include <numeric>
#include <set>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace lotus::cfl::dynamic_dyck::primary_component {
namespace {
using Node = SparsifiedConnectivity::Node;
constexpr Node NONE = std::numeric_limits<Node>::max();
using Pair = std::pair<Node, Node>;
Pair canonical(Node a, Node b) { return {std::min(a, b), std::max(a, b)}; }
struct Block {
  std::array<std::unique_ptr<Block>, 4> child;
  std::vector<Pair> forest;
  std::uint64_t count = 0; // Only leaves store original support counts.
};
struct Arc {
  Node to, next;
};
} // namespace

struct SparsifiedConnectivity::Impl {
  bool healthy = true;
  std::unique_ptr<Block> root;
  std::vector<Node> representatives;
  // Scratch arrays use dense original vertex IDs; no hashing, sorting or
  // dictionary lookup is used to compute certificates. Only touched entries
  // are reset, so a small block never pays for the full universe.
  std::vector<Node> head;
  std::vector<unsigned char> seen;
  Statistics stats;

  template <class F> auto mutate(F &&f) -> decltype(f()) {
    healthy = false;
    auto answer = f();
    healthy = true;
    return answer;
  }
  void check(Node v) const {
    if (v >= representatives.size())
      throw std::out_of_range("unknown sparsified-connectivity vertex");
  }
  std::unique_ptr<Block> makeBlock() {
    auto block = std::make_unique<Block>();
    ++stats.blocks;
    return block;
  }

  // A spanning forest is a strong connectivity certificate: replacing a child
  // graph by its forest preserves connectivity even in the union with all its
  // siblings. Induction therefore proves the parent and root certificates.
  void rebuild(Block &block, bool publish) {
    ++stats.certificate_rebuilds;
    std::vector<Arc> arcs;
    std::vector<Node> touched, stack;
    std::size_t edges = 0;
    for (const auto &child : block.child)
      if (child)
        edges += child->forest.size();
    if (edges > std::numeric_limits<std::size_t>::max() / 2)
      throw std::length_error("certificate too large");
    arcs.reserve(2 * edges);
    touched.reserve(2 * edges);
    auto add = [&](Node a, Node b) {
      if (head[a] == NONE)
        touched.push_back(a);
      arcs.push_back({b, head[a]});
      head[a] = arcs.size() - 1;
    };
    for (const auto &child : block.child) {
      if (!child)
        continue;
      for (const Pair &e : child->forest) {
        add(e.first, e.second);
        add(e.second, e.first);
        ++stats.certificate_edges_scanned;
      }
    }
    std::vector<Pair> forest;
    forest.reserve(touched.size());
    stack.reserve(touched.size());
    if (publish) {
      // Accounted O(n) work, once at the root, not once per block.
      std::iota(representatives.begin(), representatives.end(), Node{0});
      stats.representative_writes += representatives.size();
      stats.components = representatives.size();
    }
    for (Node start : touched) {
      if (seen[start])
        continue;
      seen[start] = 1;
      stack.push_back(start);
      while (!stack.empty()) {
        const Node v = stack.back();
        stack.pop_back();
        if (publish) {
          representatives[v] = start;
          ++stats.representative_writes;
        }
        for (Node i = head[v]; i != NONE; i = arcs[i].next) {
          const Node w = arcs[i].to;
          if (seen[w])
            continue;
          seen[w] = 1;
          forest.push_back(canonical(v, w));
          stack.push_back(w);
          if (publish)
            --stats.components;
        }
      }
    }
    stats.certificate_vertices += touched.size();
    for (Node v : touched) {
      head[v] = NONE;
      seen[v] = 0;
    }
    block.forest = std::move(forest);
  }

  // The pair coordinates select exactly one child at every level. Unoccupied
  // subtrees are absent. Removing an empty path never destroys live siblings.
  bool update(std::unique_ptr<Block> &block, Node a, Node b, Node width,
              bool insert, bool top, bool &changed) {
    ++stats.path_nodes_visited;
    if (!block) {
      if (!insert)
        return false;
      block = makeBlock();
    }
    if (width == 1) {
      ++stats.support_leaves_visited;
      if (insert) {
        if (block->count == std::numeric_limits<std::uint64_t>::max())
          throw std::overflow_error("primal support count overflow");
        changed = block->count++ == 0;
        ++stats.insertions;
        if (changed) {
          ++stats.edges;
          if (a != b)
            block->forest.push_back({a, b});
        }
      } else {
        if (block->count == 0)
          return false;
        ++stats.deletions;
        changed = --block->count == 0;
        if (changed) {
          --stats.edges;
          --stats.blocks;
          block.reset();
        }
      }
      return true;
    }
    const Node half = width / 2;
    const auto slot =
        static_cast<std::size_t>((a & half ? 2U : 0U) | (b & half ? 1U : 0U));
    if (!update(block->child[slot], a, b, half, insert, false, changed))
      return false;
    if (!changed)
      return true; // Reference-count-only updates do not recompute forests.
    bool empty = true;
    for (const auto &child : block->child)
      empty = empty && !child;
    if (empty) {
      --stats.blocks;
      block.reset();
      if (top) {
        std::iota(representatives.begin(), representatives.end(), Node{0});
        stats.representative_writes += representatives.size();
        stats.components = representatives.size();
      }
    } else {
      rebuild(*block, top);
    }
    return true;
  }

  void gather(const Block *block, Node row, Node column, Node width,
              std::vector<Support> &result) const {
    if (!block)
      return;
    if (width == 1) {
      result.push_back({row, column, block->count});
      return;
    }
    const Node half = width / 2;
    for (std::size_t i = 0; i < 4; ++i)
      gather(block->child[i].get(), row + ((i & 2) ? half : 0),
             column + ((i & 1) ? half : 0), half, result);
  }
};

SparsifiedConnectivity::SparsifiedConnectivity()
    : m_impl(std::make_unique<Impl>()) {}
SparsifiedConnectivity::~SparsifiedConnectivity() = default;
SparsifiedConnectivity::SparsifiedConnectivity(
    SparsifiedConnectivity &&) noexcept = default;
SparsifiedConnectivity &
SparsifiedConnectivity::operator=(SparsifiedConnectivity &&) noexcept = default;
SparsifiedConnectivity::Impl &SparsifiedConnectivity::impl() const {
  if (!m_impl)
    throw std::logic_error("operation on moved-from SparsifiedConnectivity");
  if (!m_impl->healthy)
    throw std::logic_error(
        "SparsifiedConnectivity update failed; reconstruct before use");
  return *m_impl;
}
SparsifiedConnectivity::Node SparsifiedConnectivity::addVertex() {
  Impl &d = impl();
  return d.mutate([&] {
    const Node n = d.representatives.size();
    if (n == NONE)
      throw std::length_error("too many primary vertices");
    if (n == d.stats.universe) {
      if (d.stats.universe > NONE / 2)
        throw std::length_error("primary universe overflow");
      if (d.root) {
        auto root = d.makeBlock();
        root->forest = d.root->forest;
        root->child[0] = std::move(d.root);
        d.root = std::move(root);
      }
      d.stats.universe *= 2;
      ++d.stats.levels;
    }
    d.representatives.push_back(n);
    d.head.push_back(NONE);
    d.seen.push_back(0);
    ++d.stats.vertices;
    ++d.stats.components;
    return n;
  });
}
void SparsifiedConnectivity::insertEdge(Node a, Node b) {
  Impl &d = impl();
  d.check(a);
  d.check(b);
  if (a > b)
    std::swap(a, b);
  d.mutate([&] {
    bool changed = false;
    d.update(d.root, a, b, d.stats.universe, true, true, changed);
    return true;
  });
}
bool SparsifiedConnectivity::deleteEdge(Node a, Node b) {
  Impl &d = impl();
  d.check(a);
  d.check(b);
  if (a > b)
    std::swap(a, b);
  return d.mutate([&] {
    bool changed = false;
    return d.update(d.root, a, b, d.stats.universe, false, true, changed);
  });
}
SparsifiedConnectivity::Node
SparsifiedConnectivity::representative(Node v) const {
  const Impl &d = impl();
  d.check(v);
  return d.representatives[v];
}
bool SparsifiedConnectivity::connected(Node a, Node b) const {
  return representative(a) == representative(b);
}
SparsifiedConnectivity::Statistics SparsifiedConnectivity::statistics() const {
  return impl().stats;
}
std::vector<SparsifiedConnectivity::Support>
SparsifiedConnectivity::supports() const {
  const Impl &d = impl();
  std::vector<Support> result;
  result.reserve(d.stats.edges);
  d.gather(d.root.get(), 0, 0, d.stats.universe, result);
  std::sort(result.begin(), result.end(),
            [](const Support &a, const Support &b) {
              return std::tie(a.first, a.second) < std::tie(b.first, b.second);
            });
  return result;
}

bool SparsifiedConnectivity::validate(std::string *error) const {
  const Impl &d = impl();
  try {
    const Node n = d.representatives.size();
    if (n != d.stats.vertices || d.head.size() != n || d.seen.size() != n ||
        d.stats.universe < n ||
        (d.stats.universe & (d.stats.universe - 1)) != 0)
      throw std::logic_error(
          "invalid sparsification universe/scratch dimensions");
    for (Node v = 0; v < n; ++v)
      if (d.head[v] != NONE || d.seen[v] != 0)
        throw std::logic_error("uncleared certificate scratch state");

    // Deliberately independent diagnostic implementation: ordered maps and
    // plain BFS, rather than the dense-array certificate builder above.
    auto partition = [](const std::vector<Pair> &edges, bool require_forest) {
      std::map<Node, std::vector<Node>> adj;
      for (const auto &e : edges) {
        adj[e.first].push_back(e.second);
        adj[e.second].push_back(e.first);
      }
      std::map<Node, Node> labels;
      std::size_t components = 0;
      for (const auto &entry : adj) {
        const Node start = entry.first;
        if (labels.count(start))
          continue;
        ++components;
        std::vector<Node> todo{start};
        labels[start] = start;
        while (!todo.empty()) {
          const Node v = todo.back();
          todo.pop_back();
          for (Node w : adj.at(v))
            if (labels.emplace(w, start).second)
              todo.push_back(w);
        }
      }
      if (require_forest && edges.size() != labels.size() - components)
        throw std::logic_error("certificate is not a forest");
      return labels;
    };
    std::size_t blocks = 0, leaves = 0;
    auto visit = [&](auto &&self, const Block *block, Node row, Node column,
                     Node width) -> void {
      if (!block)
        return;
      ++blocks;
      std::vector<Pair> children;
      if (width == 1) {
        ++leaves;
        if (!block->count || row > column || column >= n)
          throw std::logic_error("invalid support leaf");
        for (const auto &child : block->child)
          if (child)
            throw std::logic_error("support leaf has children");
        if (row != column)
          children.push_back({row, column});
      } else {
        if (block->count)
          throw std::logic_error("non-leaf contains support count");
        bool nonempty = false;
        const Node half = width / 2;
        for (std::size_t i = 0; i < 4; ++i) {
          const auto &child = block->child[i];
          self(self, child.get(), row + ((i & 2) ? half : 0),
               column + ((i & 1) ? half : 0), half);
          if (child) {
            nonempty = true;
            children.insert(children.end(), child->forest.begin(),
                            child->forest.end());
          }
        }
        if (!nonempty)
          throw std::logic_error("empty internal block retained");
      }
      const std::set<Pair> available(children.begin(), children.end());
      for (const Pair &e : block->forest)
        if (e.first >= e.second || !available.count(e))
          throw std::logic_error("certificate contains unsupported edge");
      if (partition(children, false) != partition(block->forest, true))
        throw std::logic_error("certificate does not span children");
    };
    visit(visit, d.root.get(), 0, 0, d.stats.universe);
    if (blocks != d.stats.blocks || leaves != d.stats.edges)
      throw std::logic_error("sparsification storage counters disagree");
    const auto labels =
        partition(d.root ? d.root->forest : std::vector<Pair>{}, true);
    std::map<Node, Node> correspondence;
    std::set<Node> actual_representatives;
    for (Node v = 0; v < n; ++v) {
      const Node r = d.representatives[v];
      const Node expected = labels.count(v) ? labels.at(v) : v;
      if (r >= n || d.representatives[r] != r ||
          (labels.count(r) ? labels.at(r) : r) != expected)
        throw std::logic_error("invalid component representative");
      const auto found = correspondence.emplace(expected, r);
      if (found.first->second != r)
        throw std::logic_error("inconsistent component representatives");
      actual_representatives.insert(r);
    }
    if (actual_representatives.size() != d.stats.components)
      throw std::logic_error("primary component count mismatch");
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
