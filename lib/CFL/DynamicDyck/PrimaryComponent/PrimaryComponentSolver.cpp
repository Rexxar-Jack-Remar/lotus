// SPDX-License-Identifier: MIT
#include "CFL/DynamicDyck/PrimaryComponent/PrimaryConnectivity.h"
#include "CFL/DynamicDyck/PrimaryComponent/PrimaryComponentSolver.h"
#include "CFL/DynamicDyck/PrimaryComponent/SparsifiedConnectivity.h"
#ifdef LOTUS_PRIMARY_COMPONENT_TEST_HOOKS
#include "PrimaryComponentTestAccess.h"
#endif

#include <algorithm>
#include <deque>
#include <iterator>
#include <limits>
#include <list>
#include <map>
#include <set>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace lotus::cfl::dynamic_dyck {
namespace {
using Node = std::size_t;
constexpr Node NONE = std::numeric_limits<Node>::max();
using NodeList = std::list<Node>;
struct Key {
  Node source;
  Label label;
  bool operator==(const Key &other) const {
    return source == other.source && label == other.label;
  }
};
struct CountRecord {
  std::uint64_t count;
  NodeList::iterator position;
};
struct OutGroup {
  NodeList targets; // Head = most recent, tail = InPrimary's witness.
  std::map<Node, CountRecord> count;
};
struct NodeData {
  Node parent;
  std::size_t size = 1;
  NodeList members;
  std::map<Label, OutGroup> out_edges;
  std::map<Label, std::set<Node>> in_primary;
  std::map<Label, NodeList> edges; // Algorithm 1's compressed Edges.
  std::set<Label> queued_labels;
  bool target_mark = false, affected_mark = false, primary_mark = false;
  explicit NodeData(Node node) : parent(node), members{node} {}
};
Edge closing(Edge edge) {
  switch (edge.kind) {
  case Parenthesis::Close:
    return edge;
  case Parenthesis::Open:
    return {edge.target, edge.source, edge.label, Parenthesis::Close};
  }
  throw std::invalid_argument("invalid dynamic-Dyck parenthesis kind");
}
void checkSemantics(PrimaryComponentEdgeSemantics semantics) {
  switch (semantics) {
  case PrimaryComponentEdgeSemantics::ReferenceCounted:
  case PrimaryComponentEdgeSemantics::Set:
    return;
  }
  throw std::invalid_argument("invalid PrimaryComponent edge semantics");
}
// Runtime selection is outside the Dyck algorithm. In Deterministic mode no
// unordered container or random balancing is reached on any update/query path.
class PrimaryBackend {
  using HDT = primary_component::PrimaryConnectivity;
  using Deterministic = primary_component::SparsifiedConnectivity;
  std::unique_ptr<HDT> hdt;
  std::unique_ptr<Deterministic> deterministic;

public:
  explicit PrimaryBackend(PrimaryComponentConnectivityBackend backend) {
    switch (backend) {
    case PrimaryComponentConnectivityBackend::HDT:
      hdt = std::make_unique<HDT>();
      break;
    case PrimaryComponentConnectivityBackend::Deterministic:
      deterministic = std::make_unique<Deterministic>();
      break;
    default:
      throw std::invalid_argument(
          "invalid PrimaryComponent connectivity backend");
    }
  }
  Node addVertex() {
    return hdt ? hdt->addVertex() : deterministic->addVertex();
  }
  void insertEdge(Node a, Node b) {
    if (hdt)
      hdt->insertEdge(a, b);
    else
      deterministic->insertEdge(a, b);
  }
  bool deleteEdge(Node a, Node b) {
    return hdt ? hdt->deleteEdge(a, b) : deterministic->deleteEdge(a, b);
  }
  Node representative(Node a) const {
    return hdt ? hdt->representative(a) : deterministic->representative(a);
  }
  bool validate(std::string *error) const {
    return hdt ? hdt->validate(error) : deterministic->validate(error);
  }
  std::vector<HDT::Support> supports() const {
    if (hdt)
      return hdt->supports();
    std::vector<HDT::Support> result;
    for (const auto &e : deterministic->supports())
      result.push_back({e.first, e.second, e.count});
    return result;
  }
  void fill(PrimaryComponentDiagnostics &result) const {
    if (hdt) {
      const auto p = hdt->statistics();
      result.primal_insertions = p.insertions;
      result.primal_deletions = p.deletions;
      result.primal_replacements = p.replacements;
      result.primal_promotions = p.promotions;
      result.primal_scanned_non_tree = p.scanned_non_tree;
      result.primal_relabeled_vertices = p.relabeled_vertices;
      result.primal_edges = p.edges;
      result.primary_components = p.components;
      result.primal_levels = p.levels;
    } else {
      const auto p = deterministic->statistics();
      result.primal_insertions = p.insertions;
      result.primal_deletions = p.deletions;
      result.primal_edges = p.edges;
      result.primary_components = p.components;
      result.primal_levels = p.levels;
      result.certificate_universe = p.universe;
      result.certificate_blocks = p.blocks;
      result.certificate_rebuilds = p.certificate_rebuilds;
      result.certificate_edges_scanned = p.certificate_edges_scanned;
      result.certificate_vertices = p.certificate_vertices;
      result.primal_representative_writes = p.representative_writes;
      result.primal_support_leaves_visited = p.support_leaves_visited;
      result.primal_path_nodes_visited = p.path_nodes_visited;
    }
  }
};
} // namespace

class PrimaryComponentSolver::Impl {
public:
  PrimaryComponentEdgeSemantics semantics;
  bool healthy = true, needs_cache = false;
  std::map<Vertex, Node> ids;
  std::vector<Vertex> vertices;
  std::vector<std::unique_ptr<NodeData>> nodes;
  PrimaryComponentConnectivityBackend backend;
  PrimaryBackend primary;
  std::deque<Key> queue;
  Statistics stats;
  mutable PrimaryComponentDiagnostics diag;
#ifdef LOTUS_PRIMARY_COMPONENT_TEST_HOOKS
  primary_component::TestAccess::Observer observer;
  primary_component::TestSnapshot
  snapshot(primary_component::TestPhase phase,
           const std::vector<Node> &affected = {}) const;
  void notify(primary_component::TestPhase phase,
              const std::vector<Node> &affected = {});
#endif

  explicit Impl(PrimaryComponentEdgeSemantics mode,
                PrimaryComponentConnectivityBackend choice)
      : semantics(mode), backend(choice), primary(choice) {
    checkSemantics(mode);
  }

  // Allocation failures during an update are not transactionally recoverable.
  // Fail closed: subsequent queries throw, rather than returning partial
  // results.
  template <typename Function>
  auto mutate(Function &&function) -> decltype(function()) {
    healthy = false;
    auto result = function();
    healthy = true;
    return result;
  }
  bool addVertex(Vertex vertex) {
    if (ids.count(vertex))
      return false;
    const Node node = vertices.size();
    if (node == NONE)
      throw std::length_error("too many PrimaryComponent vertices");
    ids.emplace(vertex, node);
    vertices.push_back(vertex);
    nodes.push_back(std::make_unique<NodeData>(node));
    if (primary.addVertex() != node)
      throw std::logic_error("primary and Dyck vertex numbering disagree");
    ++stats.components;
    return true;
  }
  Node find(Node node) const {
    Node root = node;
    while (nodes[root]->parent != root) {
      root = nodes[root]->parent;
      ++diag.dscc_parent_steps;
    }
    while (node != root) {
      const Node next = nodes[node]->parent;
      nodes[node]->parent = root;
      node = next;
    }
    return root;
  }
  // Flatten in one no-union pass, as suggested in Section 2.2. This pass is
  // O(n): each non-root parent is shortened at most once. It also starts the
  // next update's union/find batch from stars rather than an inherited forest.
  void cacheRepresentatives() {
    if (backend == PrimaryComponentConnectivityBackend::Deterministic &&
        needs_cache) {
      for (Node v = 0; v < nodes.size(); ++v) {
        nodes[v]->parent = find(v);
        ++diag.dscc_cache_vertices;
      }
      needs_cache = false;
    }
  }
  Node queryRoot(Node node) const {
    return backend == PrimaryComponentConnectivityBackend::Deterministic
               ? nodes[node]->parent
               : find(node);
  }
  void checkIndex(Node node) const {
    if (node >= nodes.size())
      throw std::out_of_range("unknown PrimaryComponent vertex index");
  }
  Node lookup(Vertex vertex) const {
    const auto found = ids.find(vertex);
    if (found == ids.end())
      throw std::out_of_range("unknown PrimaryComponent vertex");
    return found->second;
  }
  void enqueue(Node source, Label label) {
    const Key key{source, label};
    if (nodes[source]->queued_labels.insert(label).second)
      queue.push_back(key);
  }
  void enqueueIfNeeded(Node source, Label label) {
    const auto found = nodes[source]->edges.find(label);
    if (found != nodes[source]->edges.end() && found->second.size() >= 2)
      enqueue(source, label);
  }

  // Union by size, with constant-time splicing of member and edge lists.
  // Both inputs are roots; root is chosen as a largest member of the target
  // set.
  void uniteTo(Node root, Node other) {
    needs_cache = true;
    NodeData &dst = *nodes[root], &src = *nodes[other];
    src.parent = root;
    dst.size += src.size;
    src.size = 0;
    dst.members.splice(dst.members.end(), src.members);
    for (auto &entry : src.edges) {
      NodeList &list = dst.edges[entry.first];
      list.splice(list.end(), entry.second);
      enqueueIfNeeded(root, entry.first);
    }
    src.edges.clear();
    --stats.components;
    ++stats.merges;
  }

  // Algorithm 1, lines 12-29. Detaching the consumed list BEFORE merging is
  // equivalent to the paper's special self-loop branch (lines 22-25), but also
  // permits choosing a union-by-size root even when it equals the source.
  void fixpoint() {
    while (!queue.empty()) {
      const Key key = queue.front();
      queue.pop_front();
      nodes[key.source]->queued_labels.erase(key.label);
      ++diag.fixpoint_items;
      if (find(key.source) != key.source)
        continue; // A superseded representative; its lists were moved on union.
      auto &out = nodes[key.source]->edges;
      const auto found = out.find(key.label);
      if (found == out.end() || found->second.size() < 2)
        continue;
      NodeList pending;
      pending.splice(pending.end(), found->second);
      out.erase(found);
      std::vector<Node> targets;
      Node root = NONE;
      for (Node target : pending) {
        target = find(target);
        ++diag.fixpoint_targets;
        if (!nodes[target]->target_mark) {
          nodes[target]->target_mark = true;
          targets.push_back(target);
        }
        if (root == NONE || nodes[target]->size > nodes[root]->size ||
            (nodes[target]->size == nodes[root]->size && target < root))
          root = target;
      }
      for (Node target : targets)
        nodes[target]->target_mark = false;
      for (Node target : targets)
        if (target != root)
          uniteTo(root, target);
      const Node source = find(key.source);
      // If source merged into a target, preserve the other targets' outgoing
      // lists and append the new self-loop; never overwrite those lists.
      nodes[source]->edges[key.label].push_back(root);
      enqueueIfNeeded(source, key.label);
    }
  }

  void removePrimaryWitness(Node target, Label label, Node source) {
    auto &incoming = nodes[target]->in_primary;
    const auto found = incoming.find(label);
    if (found == incoming.end() || found->second.erase(source) != 1)
      throw std::logic_error("missing InPrimary tail witness");
    if (found->second.empty())
      incoming.erase(found);
  }

  // Algorithm 2. Count and the list-position index share an ordered-map record.
  bool insert(Edge edge) {
    addVertex(edge.source);
    addVertex(edge.target);
    const Node u = ids.at(edge.source), v = ids.at(edge.target);
    OutGroup &group = nodes[u]->out_edges[edge.label];
    const auto old = group.count.find(v);
    if (old != group.count.end()) {
      if (semantics == PrimaryComponentEdgeSemantics::Set)
        return false;
      if (old->second.count == std::numeric_limits<std::uint64_t>::max() ||
          diag.edge_references == std::numeric_limits<std::uint64_t>::max())
        throw std::overflow_error(
            "PrimaryComponent edge reference count overflow");
      ++old->second.count;
      ++diag.edge_references;
      ++stats.insertions;
      return true;
    }
    if (diag.edge_references == std::numeric_limits<std::uint64_t>::max())
      throw std::overflow_error(
          "PrimaryComponent total reference count overflow");
    if (group.targets.empty())
      nodes[v]->in_primary[edge.label].insert(u);
    else
      primary.insertEdge(v, group.targets.front());
    group.targets.push_front(v);
    group.count.emplace(v, CountRecord{1, group.targets.begin()});
    ++stats.edges;
    ++stats.insertions;
    ++diag.edge_references;
    const Node source = find(u);
    nodes[source]->edges[edge.label].push_back(v);
    enqueueIfNeeded(source, edge.label);
#ifdef LOTUS_PRIMARY_COMPONENT_TEST_HOOKS
    notify(primary_component::TestPhase::InsertionPrepared);
#endif
    fixpoint();
    cacheRepresentatives();
    return true;
  }

  // Algorithm 4. The three phases deliberately finish in order: inspect the
  // OLD DSCC partition, invalidate summaries, split ALL affected DSCCs, then
  // reconstruct summaries. No lookup mixes old and partially split roots.
  void makePrimary(Node deleted_source, Node deleted_target,
                   Label deleted_label) {
    ++diag.make_primary_calls;
    std::vector<Node> affected_roots;
    std::deque<Node> search;
    const Node start =
        find(deleted_target); // It is v, NOT u, for a closing edge.
    affected_roots.push_back(start);
    nodes[start]->affected_mark = true;
    search.push_back(start);
    while (!search.empty()) {
      const Node root = search.front();
      search.pop_front();
      struct Witness {
        Node target;
        bool followed = false;
      };
      std::map<Label, Witness> witnesses;
      for (Node member : nodes[root]->members) {
        for (const auto &entry : nodes[member]->out_edges) {
          const Label label = entry.first;
          const NodeList &list = entry.second.targets;
          auto witness =
              witnesses.emplace(label, Witness{list.front(), false}).first;
          if (witness->second.followed)
            continue;
          const Node first = list.front();
          ++diag.sampled_out_targets;
          // Algorithm 4 line 7 requires distinct TARGET NODES, not two edge
          // references, two different sources, or two different DSCC roots.
          if (first != witness->second.target || list.size() >= 2) {
            Node second = first;
            if (list.size() >= 2) {
              second = *std::next(list.begin());
              ++diag.sampled_out_targets;
            }
            const Node next = find(second);
            witness->second.followed = true;
            if (!nodes[next]->affected_mark) {
              nodes[next]->affected_mark = true;
              affected_roots.push_back(next);
              search.push_back(next);
            }
          }
        }
      }
    }

    std::vector<Node> affected;
    for (Node root : affected_roots)
      affected.insert(affected.end(), nodes[root]->members.begin(),
                      nodes[root]->members.end());
    diag.affected_components += affected_roots.size();
    diag.affected_vertices += affected.size();
#ifdef LOTUS_PRIMARY_COMPONENT_TEST_HOOKS
    notify(primary_component::TestPhase::AffectedDiscovered, affected_roots);
#endif

    // Explicit deletion bookkeeping: if the removed edge was the last outgoing
    // witness, InPrimary no longer lists its source. Clear this compressed slot
    // even in that case; otherwise the old target survives as a ghost edge.
    // All surviving same-label edges of this OLD source DSCC enter the seeded
    // target DSCC and are therefore reconstructed by phase 3 below.
    nodes[find(deleted_source)]->edges.erase(deleted_label);
    for (Node t : affected) {
      for (const auto &incoming : nodes[t]->in_primary) {
        for (Node s : incoming.second) {
          ++diag.in_primary_visits;
          const Node x = find(s);
          if (!nodes[x]->affected_mark)
            nodes[x]->edges.erase(incoming.first);
        }
      }
    }
    for (Node t : affected) {
      nodes[t]->members.clear();
      nodes[t]->edges.clear();
      nodes[t]->size = 0;
    }
    std::vector<Node> primary_roots;
    for (Node t : affected) {
      const Node r = primary.representative(t);
      nodes[t]->parent = r;
      nodes[r]->members.push_back(t);
      ++nodes[r]->size;
      if (!nodes[r]->primary_mark) {
        nodes[r]->primary_mark = true;
        primary_roots.push_back(r);
      }
    }
    if (primary_roots.size() < affected_roots.size())
      throw std::logic_error("primary partition does not refine old DSCCs");
    stats.components += primary_roots.size() - affected_roots.size();
    stats.splits += primary_roots.size() - affected_roots.size();

    for (Node t : affected) {
      const Node r = find(t);
      // Lines 27-30: one incoming witness per (original source, label).
      for (const auto &incoming : nodes[t]->in_primary) {
        for (Node s : incoming.second) {
          ++diag.in_primary_visits;
          const Node x = find(s);
          nodes[x]->edges[incoming.first].push_back(t);
          enqueueIfNeeded(x, incoming.first);
          ++diag.rebuilt_summary_entries;
        }
      }
      // Lines 31-34: retain outgoing edges into UNAFFECTED DSCCs too.
      for (const auto &out : nodes[t]->out_edges) {
        const Node y = out.second.targets.front();
        if (!nodes[find(y)]->primary_mark) {
          nodes[r]->edges[out.first].push_back(y);
          // Also normalize duplicate summaries of an unaffected target. This
          // cannot merge distinct unaffected DSCCs, and keeps |Edges[r][a]|<=1
          // at every public operation boundary, as used in the paper's bound.
          enqueueIfNeeded(r, out.first);
          ++diag.rebuilt_summary_entries;
        }
      }
    }
#ifdef LOTUS_PRIMARY_COMPONENT_TEST_HOOKS
    notify(primary_component::TestPhase::DeletionPrepared, affected_roots);
#endif
    for (Node r : affected_roots)
      nodes[r]->affected_mark = false;
    for (Node r : primary_roots)
      nodes[r]->primary_mark = false;
  }

  // Algorithm 3: at most two support deletions and one support insertion.
  bool erase(Edge edge) {
    const auto source = ids.find(edge.source), target = ids.find(edge.target);
    if (source == ids.end() || target == ids.end())
      return false;
    const Node u = source->second, v = target->second;
    auto &out = nodes[u]->out_edges;
    const auto group_it = out.find(edge.label);
    if (group_it == out.end())
      return false;
    OutGroup &group = group_it->second;
    const auto found = group.count.find(v);
    if (found == group.count.end())
      return false;
    ++stats.deletions;
    --diag.edge_references;
    if (--found->second.count != 0)
      return true;

    const auto position = found->second.position;
    const auto next = std::next(position);
    const Node before =
        position == group.targets.begin() ? NONE : *std::prev(position);
    const Node after = next == group.targets.end() ? NONE : *next;
    if (after == NONE) {
      removePrimaryWitness(v, edge.label, u);
      if (before != NONE)
        nodes[before]->in_primary[edge.label].insert(u);
    }
    for (Node neighbor : {before, after}) {
      if (neighbor != NONE && !primary.deleteEdge(v, neighbor))
        throw std::logic_error("missing sparse primal support");
    }
    group.targets.erase(position);
    group.count.erase(found);
    if (group.targets.empty())
      out.erase(group_it); // Do not retain empty buckets for deleted labels.
    if (before != NONE && after != NONE)
      primary.insertEdge(before, after);
    --stats.edges;
    makePrimary(u, v, edge.label);
    fixpoint();
    cacheRepresentatives();
    return true;
  }
};

#ifdef LOTUS_PRIMARY_COMPONENT_TEST_HOOKS
primary_component::TestSnapshot PrimaryComponentSolver::Impl::snapshot(
    primary_component::TestPhase phase,
    const std::vector<Node> &affected) const {
  primary_component::TestSnapshot result;
  result.phase = phase;
  result.vertices = vertices;
  result.affected = affected;
  // Do not call find(): observing must not flatten the implementation or hide
  // a cache/parent invariant failure. This deliberately follows parents
  // read-only.
  for (Node v = 0; v < nodes.size(); ++v) {
    result.parent.push_back(nodes[v]->parent);
    Node root = v;
    std::size_t steps = 0;
    while (nodes[root]->parent != root) {
      root = nodes[root]->parent;
      if (++steps > nodes.size())
        throw std::logic_error("snapshot parent cycle");
    }
    result.dscc.push_back(root);
    result.primary.push_back(primary.representative(v));
    for (const auto &entry : nodes[v]->out_edges)
      result.out_edges.push_back(
          {v,
           entry.first,
           {entry.second.targets.begin(), entry.second.targets.end()}});
    for (const auto &entry : nodes[v]->in_primary)
      result.in_primary.push_back(
          {v, entry.first, {entry.second.begin(), entry.second.end()}});
    for (const auto &entry : nodes[v]->edges)
      result.summaries.push_back(
          {v, entry.first, {entry.second.begin(), entry.second.end()}});
  }
  for (const auto &key : queue)
    result.queue.push_back({key.source, key.label});
  return result;
}
void PrimaryComponentSolver::Impl::notify(primary_component::TestPhase phase,
                                          const std::vector<Node> &affected) {
  if (observer)
    observer(snapshot(phase, affected));
}
void primary_component::TestAccess::observe(PrimaryComponentSolver &solver,
                                            Observer observer) {
  solver.impl().observer = std::move(observer);
}
primary_component::TestSnapshot
primary_component::TestAccess::snapshot(const PrimaryComponentSolver &solver) {
  return solver.impl().snapshot(TestPhase::Boundary);
}
#endif

PrimaryComponentSolver::PrimaryComponentSolver(
    PrimaryComponentEdgeSemantics semantics,
    PrimaryComponentConnectivityBackend backend)
    : m_impl(std::make_unique<Impl>(semantics, backend)) {}
PrimaryComponentSolver::PrimaryComponentSolver(
    const Graph &graph, PrimaryComponentEdgeSemantics semantics,
    PrimaryComponentConnectivityBackend backend)
    : PrimaryComponentSolver(semantics, backend) {
  for (Vertex vertex : graph.vertices)
    addVertex(vertex);
  for (Edge edge : graph.edges)
    insertEdge(edge);
}
PrimaryComponentSolver::~PrimaryComponentSolver() = default;
PrimaryComponentSolver::PrimaryComponentSolver(
    PrimaryComponentSolver &&) noexcept = default;
PrimaryComponentSolver &
PrimaryComponentSolver::operator=(PrimaryComponentSolver &&) noexcept = default;
PrimaryComponentSolver::Impl &PrimaryComponentSolver::impl() const {
  if (!m_impl)
    throw std::logic_error("operation on moved-from PrimaryComponentSolver");
  if (!m_impl->healthy)
    throw std::logic_error("PrimaryComponentSolver update failed; "
                           "reconstruct the instance before use");
  return *m_impl;
}
bool PrimaryComponentSolver::addVertex(Vertex vertex) {
  Impl &d = impl();
  return d.mutate([&] { return d.addVertex(vertex); });
}
bool PrimaryComponentSolver::insertEdge(Edge edge) {
  edge = closing(edge); // Validate before changing any state.
  Impl &d = impl();
  return d.mutate([&] { return d.insert(edge); });
}
bool PrimaryComponentSolver::deleteEdge(Edge edge) {
  edge = closing(edge);
  Impl &d = impl();
  return d.mutate([&] { return d.erase(edge); });
}
bool PrimaryComponentSolver::apply(const Update &update) {
  switch (update.kind) {
  case UpdateKind::Insert:
    return insertEdge(update.edge);
  case UpdateKind::Delete:
    return deleteEdge(update.edge);
  }
  throw std::invalid_argument("invalid dynamic-Dyck update kind");
}
bool PrimaryComponentSolver::connected(Vertex source, Vertex target) const {
  const Impl &d = impl();
  const auto a = d.ids.find(source), b = d.ids.find(target);
  return a != d.ids.end() && b != d.ids.end() &&
         d.queryRoot(a->second) == d.queryRoot(b->second);
}
PrimaryComponentSolver::VertexIndex
PrimaryComponentSolver::vertexIndex(Vertex vertex) const {
  return impl().lookup(vertex);
}
Vertex PrimaryComponentSolver::vertexAt(VertexIndex index) const {
  const Impl &d = impl();
  d.checkIndex(index);
  return d.vertices[index];
}
bool PrimaryComponentSolver::connectedByIndex(VertexIndex source,
                                              VertexIndex target) const {
  const Impl &d = impl();
  d.checkIndex(source);
  d.checkIndex(target);
  return d.queryRoot(source) == d.queryRoot(target);
}
Vertex PrimaryComponentSolver::representativeByIndex(VertexIndex vertex) const {
  const Impl &d = impl();
  d.checkIndex(vertex);
  return d.vertices[d.queryRoot(vertex)];
}
Vertex
PrimaryComponentSolver::primaryRepresentativeByIndex(VertexIndex vertex) const {
  const Impl &d = impl();
  d.checkIndex(vertex);
  return d.vertices[d.primary.representative(vertex)];
}
Vertex PrimaryComponentSolver::representative(Vertex vertex) const {
  const Impl &d = impl();
  return d.vertices[d.queryRoot(d.lookup(vertex))];
}
Vertex PrimaryComponentSolver::primaryRepresentative(Vertex vertex) const {
  const Impl &d = impl();
  return d.vertices[d.primary.representative(d.lookup(vertex))];
}
std::vector<std::vector<Vertex>> PrimaryComponentSolver::components() const {
  const Impl &d = impl();
  std::vector<std::vector<Vertex>> result;
  for (const auto &node : d.nodes) {
    if (node->members.empty())
      continue;
    result.emplace_back();
    for (Node member : node->members)
      result.back().push_back(d.vertices[member]);
    std::sort(result.back().begin(), result.back().end());
  }
  std::sort(result.begin(), result.end());
  return result;
}
std::vector<PrimaryComponentCountedEdge>
PrimaryComponentSolver::edgeCounts() const {
  const Impl &d = impl();
  std::vector<PrimaryComponentCountedEdge> result;
  result.reserve(d.stats.edges);
  for (Node u = 0; u < d.nodes.size(); ++u)
    for (const auto &group : d.nodes[u]->out_edges)
      for (const auto &target : group.second.count)
        result.push_back({{d.vertices[target.first], d.vertices[u], group.first,
                           Parenthesis::Open},
                          target.second.count});
  std::sort(result.begin(), result.end(),
            [](const PrimaryComponentCountedEdge &a,
               const PrimaryComponentCountedEdge &b) {
              return std::tie(a.edge.source, a.edge.target, a.edge.label) <
                     std::tie(b.edge.source, b.edge.target, b.edge.label);
            });
  return result;
}
Graph PrimaryComponentSolver::graph() const {
  Graph result;
  result.vertices = impl().vertices;
  std::sort(result.vertices.begin(), result.vertices.end());
  for (const auto &entry : edgeCounts())
    result.edges.push_back(entry.edge);
  return result;
}
std::uint64_t PrimaryComponentSolver::edgeMultiplicity(Edge edge) const {
  edge = closing(edge);
  const Impl &d = impl();
  const auto u = d.ids.find(edge.source), v = d.ids.find(edge.target);
  if (u == d.ids.end() || v == d.ids.end())
    return 0;
  const auto group = d.nodes[u->second]->out_edges.find(edge.label);
  if (group == d.nodes[u->second]->out_edges.end())
    return 0;
  const auto found = group->second.count.find(v->second);
  return found == group->second.count.end() ? 0 : found->second.count;
}
Statistics PrimaryComponentSolver::statistics() const {
  const Impl &d = impl();
  Statistics result = d.stats;
  result.vertices = d.vertices.size();
  return result;
}
PrimaryComponentDiagnostics PrimaryComponentSolver::diagnostics() const {
  const Impl &d = impl();
  PrimaryComponentDiagnostics result = d.diag;
  d.primary.fill(result);
  return result;
}
PrimaryComponentEdgeSemantics PrimaryComponentSolver::edgeSemantics() const {
  return impl().semantics;
}
PrimaryComponentConnectivityBackend
PrimaryComponentSolver::connectivityBackend() const {
  return impl().backend;
}

bool PrimaryComponentSolver::validate(std::string *error) const {
  const Impl &d = impl();
  try {
    std::string primary_error;
    if (!d.primary.validate(&primary_error))
      throw std::logic_error("PrimCompDS: " + primary_error);
    if (!d.queue.empty())
      throw std::logic_error("nonempty fixpoint queue at operation boundary");
    const std::size_t n = d.nodes.size();
    if (d.vertices.size() != n || d.ids.size() != n)
      throw std::logic_error("vertex map size mismatch");
    for (Node v = 0; v < n; ++v) {
      if (d.ids.at(d.vertices[v]) != v)
        throw std::logic_error("vertex map is not a bijection");
      Node p = v;
      for (std::size_t steps = 0;; ++steps) {
        if (p >= n || steps > n)
          throw std::logic_error("invalid union-find parent/cycle");
        if (p == d.nodes[p]->parent)
          break;
        p = d.nodes[p]->parent;
      }
    }
    std::vector<bool> listed(n, false);
    std::size_t components = 0, edge_count = 0;
    std::uint64_t references = 0;
    using IncomingKey = std::pair<Node, Label>;
    std::map<IncomingKey, std::set<Node>> expected_incoming, actual_incoming;
    std::map<IncomingKey, Node> expected_summary, actual_summary;
    std::map<std::pair<Node, Node>, std::uint64_t> expected_primal,
        actual_primal;
    for (Node u = 0; u < n; ++u) {
      const Node root = d.find(u);
      const NodeData &data = *d.nodes[u];
      if (!data.queued_labels.empty() || data.target_mark ||
          data.affected_mark || data.primary_mark)
        throw std::logic_error(
            "temporary worklist/mark retained at operation boundary");
      if (d.backend == PrimaryComponentConnectivityBackend::Deterministic &&
          data.parent != root)
        throw std::logic_error("DSCC representative cache is not flat");
      if (d.find(d.primary.representative(u)) != root)
        throw std::logic_error("PDSCC is not contained in DSCC");
      if (root == u) {
        ++components;
        if (data.size == 0 || data.members.size() != data.size)
          throw std::logic_error("DSCC member count mismatch");
        for (Node v : data.members) {
          if (v >= n || listed[v] || d.find(v) != u)
            throw std::logic_error("invalid DSCC member list");
          listed[v] = true;
        }
      } else if (data.size != 0 || !data.members.empty() ||
                 !data.edges.empty()) {
        throw std::logic_error("non-root retains DSCC state");
      }
      for (const auto &group : data.out_edges) {
        const OutGroup &out = group.second;
        if (out.targets.empty() || out.targets.size() != out.count.size())
          throw std::logic_error("OutEdges/Count mismatch");
        expected_incoming[{out.targets.back(), group.first}].insert(u);
        Node previous = NONE;
        std::set<Node> distinct;
        for (auto it = out.targets.begin(); it != out.targets.end(); ++it) {
          const Node v = *it;
          const auto record = out.count.find(v);
          if (v >= n || !distinct.insert(v).second ||
              record == out.count.end() || record->second.count == 0 ||
              record->second.position != it)
            throw std::logic_error("invalid Count/list-position record");
          if (d.semantics == PrimaryComponentEdgeSemantics::Set &&
              record->second.count != 1)
            throw std::logic_error(
                "set semantics retained duplicate references");
          ++edge_count;
          references += record->second.count;
          const Node target = d.find(v);
          const auto summary =
              expected_summary.emplace(IncomingKey{root, group.first}, target);
          if (summary.first->second != target)
            throw std::logic_error("DSCC partition is not saturated");
          if (d.primary.representative(v) !=
              d.primary.representative(out.targets.front()))
            throw std::logic_error("OutEdges list crosses PDSCCs");
          if (previous != NONE)
            ++expected_primal[{std::min(previous, v), std::max(previous, v)}];
          previous = v;
        }
      }
      for (const auto &entry : data.in_primary) {
        if (entry.second.empty())
          throw std::logic_error("empty InPrimary bucket retained");
        actual_incoming[{u, entry.first}] =
            std::set<Node>(entry.second.begin(), entry.second.end());
      }
      for (const auto &entry : data.edges) {
        if (entry.second.size() != 1 || entry.second.front() >= n)
          throw std::logic_error("compressed Edges slot not normalized");
        actual_summary[{u, entry.first}] = d.find(entry.second.front());
      }
    }
    if (std::find(listed.begin(), listed.end(), false) != listed.end())
      throw std::logic_error("node missing from DSCC member lists");
    for (const auto &support : d.primary.supports())
      actual_primal[{support.first, support.second}] = support.count;
    if (expected_primal != actual_primal)
      throw std::logic_error(
          "sparse chain/primal support multiplicities disagree");
    if (expected_incoming != actual_incoming)
      throw std::logic_error("InPrimary tail invariant violated");
    if (expected_summary != actual_summary)
      throw std::logic_error(
          "compressed Edges soundness/completeness violated");
    if (components != d.stats.components || edge_count != d.stats.edges ||
        references != d.diag.edge_references)
      throw std::logic_error("statistics disagree with graph state");
    if (error)
      error->clear();
    return true;
  } catch (const std::logic_error &failure) {
    if (error)
      *error = failure.what();
    return false;
  }
}
} // namespace lotus::cfl::dynamic_dyck
