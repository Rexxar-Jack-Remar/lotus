#include "IR/UseTraceSSA/Query.h"
#include <algorithm>
#include <deque>
#include <stdexcept>
#include <tuple>
#include <unordered_map>

namespace lotus {
namespace usetracessa {
ObjectUniverse::ObjectUniverse(std::vector<ObjectID> objects) : Objects(std::move(objects)) {
  std::sort(Objects.begin(), Objects.end());
  Objects.erase(std::unique(Objects.begin(), Objects.end()), Objects.end());
  if (Objects.size() >= InvalidID)
    throw std::length_error("UseTraceSSA: object universe too large");
}
std::size_t ObjectUniverse::index(ObjectID object) const {
  auto it = std::lower_bound(Objects.begin(), Objects.end(), object);
  if (it == Objects.end() || *it != object)
    throw std::out_of_range("UseTraceSSA: object outside query universe");
  return it - Objects.begin();
}
ObjectMask ObjectUniverse::mask(const ObjectSet &set) const {
  if (set.isUnknown()) return all();
  auto result = none();
  for (auto object : set.objects()) {
    auto it = std::lower_bound(Objects.begin(), Objects.end(), object);
    if (it != Objects.end() && *it == object) result.set(it - Objects.begin());
  }
  return result;
}
QueryStatus ObjectBatchResult::status(ObjectID object) const {
  auto bit = universe.index(object);
  return found.test(bit) ? QueryStatus::Found :
         unknown.test(bit) ? QueryStatus::Unknown : QueryStatus::NotFound;
}
namespace {
struct Budget {};
std::uint64_t key(ID a, ID b) { return (std::uint64_t(a) << 32) | b; }
struct ProductNode { FlowNodeID node; ID state; };
struct ProductEdge { ID from, to; FlowEdgeID original; ObjectMask objects; };
struct EffectClass { ObjectMask objects; Event events; Certainty certainty; };
struct Delta { ID id; ObjectMask objects; bool positive = false; };
struct Summary { ID from, to; ObjectMask reachable, positive; };
} // namespace

ObjectBatchResult QueryEngine::runObjects(const ObjectBatchQuery &request) const {
  const auto &q = request.query;
  const auto &u = request.universe;
  if (q.memoryObject)
    throw std::invalid_argument("UseTraceSSA: batch query cannot specify a fixed object");
  if (!q.automaton.states || q.automaton.initial >= q.automaton.states)
    throw std::invalid_argument("UseTraceSSA: invalid automaton initial state");
  ObjectBatchResult result;
  result.universe = u;
  result.found = result.notFound = result.unknown = u.none();
  auto &stats = result.statistics;
  stats.candidateObjects = u.objects().size();
  auto intersect = [&](ObjectMask a, const ObjectMask &b) {
    ++stats.maskIntersections; a &= b; return a;
  };
  auto unite = [&](ObjectMask &a, const ObjectMask &b) {
    ++stats.maskUnions; a |= b;
  };
  auto difference = [](ObjectMask a, const ObjectMask &b) { a.reset(b); return a; };
  auto bound = [](std::size_t n, std::size_t max) { if (max && n >= max) throw Budget{}; };
  std::size_t work = 0;
  auto tick = [&] { bound(work++, q.maxWork); };
  std::vector<bool> accepts(q.automaton.states), sinks(G.nodes().size()), traps(G.nodes().size());
  for (auto state : q.automaton.accepting) {
    if (state >= accepts.size()) throw std::invalid_argument("UseTraceSSA: accepting state");
    accepts[state] = true;
  }
  for (auto n : q.sources) G.node(n);
  for (auto n : q.sinks) { G.node(n); sinks[n] = true; }
  for (auto n : q.traps) { G.node(n); traps[n] = true; }

  // Cached only for this invocation: graph revision and universe are immutable
  // for its lifetime. Split by effect guards, never by individual object IDs.
  std::map<FlowNodeID, std::vector<EffectClass>> partitions;
  auto partition = [&](FlowNodeID id) -> const std::vector<EffectClass> & {
    auto found = partitions.find(id);
    if (found != partitions.end()) return found->second;
    const auto &n = G.node(id);
    std::vector<EffectClass> classes;
    if (!traps[id] && !u.objects().empty()) classes.push_back({u.all(), n.events, n.certainty});
    for (const auto &effect : n.effects) {
      ObjectMask guard = u.mask(effect.objects);
      std::vector<EffectClass> next;
      for (const auto &c : classes) {
        auto yes = intersect(c.objects, guard), no = difference(c.objects, guard);
        if (no.any()) next.push_back({std::move(no), c.events, c.certainty});
        if (yes.any()) next.push_back({std::move(yes), c.events | effect.events,
            c.certainty == Certainty::May || effect.certainty == Certainty::May ||
            effect.objects.isUnknown() ? Certainty::May : Certainty::Must});
      }
      // Equal semantics may be reunited; distinct effects remain intact in IR.
      std::map<std::pair<Event, Certainty>, ObjectMask> merged;
      for (const auto &c : next) {
        auto it = merged.emplace(std::make_pair(c.events, c.certainty), u.none()).first;
        unite(it->second, c.objects);
      }
      classes.clear();
      for (auto &c : merged) classes.push_back({std::move(c.second), c.first.first, c.first.second});
    }
    return partitions.emplace(id, std::move(classes)).first->second;
  };
  auto effects = [&](FlowNodeID node, ID state) {
    std::map<ID, ObjectMask> transitions;
    auto add = [&](ID next, const ObjectMask &objects) {
      if (next >= q.automaton.states)
        throw std::invalid_argument("UseTraceSSA: automaton transition out of range");
      auto it = transitions.emplace(next, u.none()).first;
      unite(it->second, objects);
    };
    for (const auto &c : partition(node)) {
      if (!hasEvent(c.events, q.trapEvents))
        add(q.automaton.transition ? q.automaton.transition(state, c.events) : state, c.objects);
      if (c.certainty == Certainty::May) add(state, c.objects);
    }
    return transitions;
  };

  // Both modes use precisely (FlowNodeID, AutomatonState). Masks are payloads,
  // not a third product dimension. Incoming deltas intersect transition labels.
  std::vector<ProductNode> pn;
  std::vector<ProductEdge> pe;
  std::vector<std::vector<ID>> pout;
  std::vector<ObjectMask> reached;
  std::unordered_map<std::uint64_t, ID> products;
  std::map<std::tuple<ID, ID, FlowEdgeID>, ID> productEdges;
  std::map<ID, ObjectMask> roots;
  std::deque<Delta> pending;
  ObjectMask incomplete = G.complete() ? u.none() : u.all();
  bool limited = false, hasContext = false;
  auto product = [&](FlowNodeID node, ID state) -> ID {
    auto k = key(node, state);
    auto it = products.find(k);
    if (it != products.end()) return it->second;
    bound(pn.size(), q.maxProductStates);
    if (pn.size() >= InvalidID / 4) throw std::length_error("UseTraceSSA: product too large");
    ID id = pn.size();
    products.emplace(k, id); pn.push_back({node, state});
    pout.emplace_back(); reached.push_back(u.none());
    return id;
  };
  auto reach = [&](ID id, const ObjectMask &objects) {
    auto delta = difference(objects, reached[id]);
    if (delta.none()) return;
    unite(reached[id], delta); ++stats.nonemptyDeltas;
    pending.push_back({id, std::move(delta)});
  };
  try {
    for (auto source : q.sources) for (const auto &t : effects(source, q.automaton.initial)) {
      auto p = product(source, t.first);
      unite(roots.emplace(p, u.none()).first->second, t.second);
      reach(p, t.second);
    }
    while (!pending.empty()) {
      Delta delta = std::move(pending.front()); pending.pop_front();
      ProductNode n = pn[delta.id];
      for (auto eid : G.outgoing(n.node)) {
        tick(); ++stats.edgesExamined;
        const auto &e = G.edge(eid);
        if (!e.enabled || e.objects.empty() || (q.edgeFilter && !q.edgeFilter(e))) continue;
        auto edgeMask = u.mask(e.objects);
        auto permitted = intersect(delta.objects, edgeMask);
        if (permitted.none()) continue;
        if (e.kind == FlowKind::Thread && !q.includeThreadEdges) {
          unite(incomplete, permitted); continue;
        }
        for (const auto &t : effects(e.to, n.state)) {
          auto arriving = intersect(permitted, t.second);
          if (arriving.none()) continue;
          ID target = product(e.to, t.first);
          auto k = std::make_tuple(delta.id, target, eid);
          auto previous = productEdges.find(k);
          if (previous == productEdges.end()) {
            ID id = pe.size(); productEdges.emplace(k, id);
            pe.push_back({delta.id, target, eid, intersect(edgeMask, t.second)});
            pout[delta.id].push_back(id);
          }
          hasContext |= e.kind == FlowKind::Call || e.kind == FlowKind::Return;
          reach(target, arriving);
        }
      }
    }
  } catch (const Budget &) { limited = true; }
  stats.productStates = pn.size(); stats.productEdges = pe.size();

  // D ::= epsilon | local | D D | call_c D return_c. Sequential composition
  // intersects; alternative derivations union for the SAME relation pair.
  // Separate positive masks retain nonempty cycles even when epsilon exists.
  bool context = hasContext && q.context != ContextMode::Insensitive;
  std::vector<Summary> summaries;
  std::vector<std::vector<ID>> rout(pn.size()), rin(pn.size());
  if (context) {
    std::unordered_map<std::uint64_t, ID> relation;
    std::deque<Delta> queue;
    std::vector<std::vector<ID>> callsIn(pn.size()), returnsOut(pn.size());
    auto insert = [&](ID from, ID to, const ObjectMask &objects, bool positive) {
      if (objects.none()) return;
      auto k = key(from, to);
      auto it = relation.find(k);
      if (it == relation.end()) {
        bound(summaries.size(), q.maxSummaryPairs);
        ID id = summaries.size();
        it = relation.emplace(k, id).first;
        summaries.push_back({from, to, u.none(), u.none()});
        rout[from].push_back(id); rin[to].push_back(id);
      }
      ID id = it->second;
      auto update = [&](ObjectMask &old, bool pos) {
        auto delta = difference(objects, old);
        if (delta.none()) return;
        unite(old, delta); ++stats.nonemptyDeltas;
        queue.push_back({id, std::move(delta), pos});
      };
      update(summaries[id].reachable, false);
      if (positive) update(summaries[id].positive, true);
    };
    try {
      for (ID i = 0; i < pn.size(); ++i) insert(i, i, u.all(), false);
      for (ID i = 0; i < pe.size(); ++i) {
        const auto &e = pe[i]; auto kind = G.edge(e.original).kind;
        if (kind == FlowKind::Call) callsIn[e.to].push_back(i);
        else if (kind == FlowKind::Return) returnsOut[e.from].push_back(i);
        else insert(e.from, e.to, e.objects, true);
      }
      while (!queue.empty()) {
        Delta d = std::move(queue.front()); queue.pop_front();
        // insert() can reallocate both summaries and adjacency vectors.
        ID from = summaries[d.id].from, to = summaries[d.id].to;
        std::size_t preds = rin[from].size(), succs = rout[to].size();
        for (std::size_t i = 0; i < preds; ++i) {
          tick(); auto a = summaries[rin[from][i]];
          insert(a.from, to, intersect(a.reachable, d.objects), d.positive);
          if (!d.positive) insert(a.from, to, intersect(a.positive, d.objects), true);
        }
        for (std::size_t i = 0; i < succs; ++i) {
          tick(); auto b = summaries[rout[to][i]];
          insert(from, b.to, intersect(d.objects, b.reachable), d.positive);
          if (!d.positive) insert(from, b.to, intersect(d.objects, b.positive), true);
        }
        if (!d.positive) for (auto c : callsIn[from]) for (auto r : returnsOut[to]) {
          if (G.edge(pe[c].original).callSite != G.edge(pe[r].original).callSite) continue;
          tick();
          insert(pe[c].from, pe[r].to,
                 intersect(intersect(pe[c].objects, d.objects), pe[r].objects), true);
        }
      }
    } catch (const Budget &) { limited = true; }
  }
  stats.summaryPairs = summaries.size();

  // Existing realizable quotient: unmatched returns precede unmatched calls.
  // Delta masks belong to logical (product, phase, positive) states; no object
  // creates a separate search state or a per-object witness recipe.
  std::vector<ObjectMask> visited(pn.size() * 4, u.none());
  std::deque<Delta> queue;
  auto push = [&](ID state, const ObjectMask &objects) {
    auto delta = difference(objects, visited[state]);
    if (delta.none()) return;
    unite(visited[state], delta); ++stats.nonemptyDeltas;
    queue.push_back({state, std::move(delta)});
  };
  for (const auto &root : roots) push(root.first * 4, root.second);
  while (!queue.empty()) {
    Delta d = std::move(queue.front()); queue.pop_front();
    ID p = d.id / 4; unsigned phase = (d.id % 4) / 2; bool positive = d.id % 2;
    if (sinks[pn[p].node] && accepts[pn[p].state] && (!q.requireNonEmpty || positive)) {
      unite(result.found, d.objects);
      unite(result.foundAt.emplace(pn[p].node, u.none()).first->second, d.objects);
    }
    auto step = [&](ID target, unsigned nextPhase, bool nonempty, const ObjectMask &mask) {
      push(target * 4 + nextPhase * 2 + unsigned(positive || nonempty),
           intersect(d.objects, mask));
    };
    if (context) for (auto id : rout[p]) {
      const auto &s = summaries[id];
      step(s.to, phase, false, s.reachable);
      step(s.to, phase, true, s.positive);
    }
    for (auto id : pout[p]) {
      const auto &e = pe[id]; auto kind = G.edge(e.original).kind;
      if (!context || (kind != FlowKind::Call && kind != FlowKind::Return))
        step(e.to, phase, true, e.objects);
      else if (q.context == ContextMode::Realizable) {
        if (kind == FlowKind::Call) step(e.to, 1, true, e.objects);
        else if (kind == FlowKind::Return && !phase) step(e.to, 0, true, e.objects);
      }
    }
  }
  result.unknown = difference(limited ? u.all() : incomplete, result.found);
  result.complete = !limited && incomplete.none();
  result.notFound = difference(difference(u.all(), result.found), result.unknown);
  stats.foundObjects = result.found.count(); stats.notFoundObjects = result.notFound.count();
  stats.unknownObjects = result.unknown.count();
  result.message = limited ? "symbolic query budget exhausted; remaining objects unknown" :
      incomplete.any() ? "incomplete model or omitted thread edges" :
                            "symbolic same-object reachability complete";
  return result;
}
} // namespace usetracessa
} // namespace lotus
