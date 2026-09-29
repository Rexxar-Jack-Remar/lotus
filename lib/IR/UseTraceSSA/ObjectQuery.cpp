#include "IR/UseTraceSSA/Query.h"
#include "IR/UseTraceSSA/QueryContext.h"
#include <algorithm>
#include <deque>
#include <memory>
#include <stdexcept>
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
using Budget = detail::SearchBudgetExceeded;
// Query facts share immutable labels. TOP, BOTTOM, and unchanged masks need no
// new dense allocation; joins replace their handle instead of mutating aliases.
using Mask = std::shared_ptr<const ObjectMask>;
std::uint64_t key(ID a, ID b) { return (std::uint64_t(a) << 32) | b; }
struct ProductNode { FlowNodeID node; ID state; };
struct ProductEdge { ID from, to; FlowEdgeID original; ID transition; };
struct ProductTransition {
  FlowNodeID node;
  ID state;
  FlowEdgeID original;
  Mask objects;
  ID edge = InvalidID;
};
struct EffectClass { Mask objects; Event events; Certainty certainty; };
struct Delta { ID id; Mask objects; bool positive = false; ID proof = InvalidID; };
struct Summary { ID from, to; Mask reachable, positive; };
} // namespace

struct ObjectWitnessData {
  struct Proof { ID parent; FlowEdgeID edge; FlowNodeID node; ID state; };
  struct Acceptance { Mask objects; ID proof; };
  std::vector<Proof> proofs;
  std::map<FlowNodeID, std::vector<Acceptance>> accepted;
  std::size_t maxEdges = 0;
};

QueryResult ObjectBatchResult::witness(FlowNodeID sink, ObjectID object) const {
  QueryResult result;
  result.completion = completion;
  result.status = QueryStatus::Unknown;
  result.message = "object witness provenance was not retained";
  if (!Witnesses) return result;
  auto bit = universe.index(object);
  auto accepted = Witnesses->accepted.find(sink);
  ID proof = InvalidID;
  if (accepted != Witnesses->accepted.end())
    for (const auto &candidate : accepted->second)
      if (candidate.objects->test(bit)) { proof = candidate.proof; break; }
  if (proof == InvalidID) {
    result.status = complete ? QueryStatus::NotFound : QueryStatus::Unknown;
    result.message = "no retained witness for this sink and object";
    return result;
  }
  result.status = QueryStatus::Found;
  result.message = "witness from symbolic object tabulation";
  result.productStates = statistics.productStates;
  result.edgesExamined = statistics.edgesExamined;
  result.summaryPairs = statistics.summaryPairs;
  std::vector<ID> path;
  for (ID id = proof; id != InvalidID; id = Witnesses->proofs[id].parent)
    path.push_back(id);
  std::reverse(path.begin(), path.end());
  for (ID id : path) {
    const auto &step = Witnesses->proofs[id];
    if (step.parent != InvalidID) {
      if (Witnesses->maxEdges && result.edges.size() >= Witnesses->maxEdges) {
        result.witnessComplete = false;
        result.message += "; witness rendering truncated";
        break;
      }
      result.edges.push_back(step.edge);
    }
    result.nodes.push_back(step.node);
    result.automatonStates.push_back(step.state);
  }
  return result;
}

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
  const Mask emptyMask = std::make_shared<ObjectMask>(u.none());
  const Mask allMask = u.objects().empty() ? emptyMask :
                       std::make_shared<ObjectMask>(u.all());
  auto makeMask = [&](ObjectMask bits) -> Mask {
    if (bits.none()) return emptyMask;
    if (bits.all()) return allMask;
    return std::make_shared<ObjectMask>(std::move(bits));
  };
  auto objectMask = [&](const ObjectSet &objects) -> Mask {
    if (objects.isUnknown()) return allMask;
    if (objects.empty()) return emptyMask;
    return makeMask(u.mask(objects));
  };
  auto intersect = [&](const Mask &a, const Mask &b) -> Mask {
    ++stats.maskIntersections;
    if (a == b || b == allMask || a == emptyMask) return a;
    if (a == allMask || b == emptyMask) return b;
    ObjectMask bits = *a; bits &= *b;
    if (bits == *a) return a;
    if (bits == *b) return b;
    return makeMask(std::move(bits));
  };
  auto unite = [&](Mask &a, const Mask &b) {
    ++stats.maskUnions;
    if (a == b || a == allMask || b == emptyMask) return;
    if (a == emptyMask || b == allMask) { a = b; return; }
    ObjectMask bits = *a; bits |= *b;
    if (bits == *a) return;
    if (bits == *b) { a = b; return; }
    a = makeMask(std::move(bits));
  };
  auto difference = [&](const Mask &a, const Mask &b) -> Mask {
    if (a == b || b == allMask) return emptyMask;
    if (b == emptyMask || a == emptyMask) return a;
    ObjectMask bits = *a; bits.reset(*b);
    if (bits == *a) return a;
    return makeMask(std::move(bits));
  };
  Mask foundObjects = emptyMask;
  std::map<FlowNodeID, Mask> foundAt;
  auto bound = detail::checkSearchBudget;
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
    if (!traps[id] && !u.objects().empty()) classes.push_back({allMask, n.events, n.certainty});
    for (const auto &effect : n.effects) {
      Mask guard = objectMask(effect.objects);
      std::vector<EffectClass> next;
      for (const auto &c : classes) {
        auto yes = intersect(c.objects, guard), no = difference(c.objects, guard);
        if (no != emptyMask) next.push_back({std::move(no), c.events, c.certainty});
        if (yes != emptyMask) next.push_back({std::move(yes), c.events | effect.events,
            c.certainty == Certainty::May || effect.certainty == Certainty::May ||
            effect.objects.isUnknown() ? Certainty::May : Certainty::Must});
      }
      // Equal semantics may be reunited; distinct effects remain intact in IR.
      std::map<std::pair<Event, Certainty>, Mask> merged;
      for (const auto &c : next) {
        auto it = merged.emplace(std::make_pair(c.events, c.certainty), emptyMask).first;
        unite(it->second, c.objects);
      }
      classes.clear();
      for (auto &c : merged)
        classes.push_back({std::move(c.second), c.first.first, c.first.second});
    }
    return partitions.emplace(id, std::move(classes)).first->second;
  };
  std::unordered_map<detail::FlowStateKey, std::map<ID, Mask>, detail::FlowStateHash> effectCache;
  auto effects = [&](FlowNodeID node, ID state) -> const std::map<ID, Mask> & {
    detail::FlowStateKey k{node, state};
    auto cached = effectCache.find(k);
    if (cached != effectCache.end()) return cached->second;
    std::map<ID, Mask> transitions;
    auto add = [&](ID next, const Mask &objects) {
      if (next >= q.automaton.states)
        throw std::invalid_argument("UseTraceSSA: automaton transition out of range");
      auto it = transitions.emplace(next, emptyMask).first;
      unite(it->second, objects);
    };
    for (const auto &c : partition(node)) {
      if (!hasEvent(c.events, q.trapEvents))
        add(q.automaton.transition ? q.automaton.transition(state, c.events) : state, c.objects);
      if (c.certainty == Certainty::May) add(state, c.objects);
    }
    return effectCache.emplace(k, std::move(transitions)).first->second;
  };

  // Both modes use precisely (FlowNodeID, AutomatonState). Masks are payloads,
  // not a third product dimension. Incoming deltas intersect transition labels.
  std::vector<ProductNode> pn;
  std::vector<ProductEdge> pe;
  std::vector<std::vector<ID>> pout;
  // Transition labels are compiled once and shared with materialized edges.
  // A target product is created only when a reachable delta uses its label.
  std::vector<std::vector<ProductTransition>> transitions;
  std::vector<Mask> omittedThreads;
  std::vector<Mask> reached;
  std::vector<bool> expanded;
  std::vector<Mask> edgeMasks(G.edges().size());
  std::vector<bool> edgeMaskReady(G.edges().size());
  std::unordered_map<detail::FlowStateKey, ID, detail::FlowStateHash> products;
  std::map<ID, Mask> roots;
  std::deque<Delta> pending;
  Mask incomplete = G.complete() ? emptyMask : allMask;
  bool limited = false, hasContext = false;
  auto product = [&](FlowNodeID node, ID state) -> ID {
    detail::FlowStateKey k{node, state};
    auto it = products.find(k);
    if (it != products.end()) return it->second;
    bound(pn.size(), q.maxProductStates, SearchStopReason::ProductStates);
    if (pn.size() >= InvalidID / 4) throw std::length_error("UseTraceSSA: product too large");
    ID id = pn.size();
    products.emplace(k, id); pn.push_back({node, state});
    pout.emplace_back(); transitions.emplace_back(); omittedThreads.push_back(emptyMask);
    reached.push_back(emptyMask); expanded.push_back(false);
    return id;
  };
  auto reach = [&](ID id, const Mask &objects) {
    auto delta = difference(objects, reached[id]);
    if (delta == emptyMask) return;
    unite(reached[id], delta); ++stats.nonemptyDeltas;
    pending.push_back({id, std::move(delta)});
  };
  auto productMask = [&](const ProductEdge &edge) -> const Mask & {
    return transitions[edge.from][edge.transition].objects;
  };
  try {
    for (auto source : q.sources) for (const auto &t : effects(source, q.automaton.initial)) {
      auto p = product(source, t.first);
      unite(roots.emplace(p, emptyMask).first->second, t.second);
      reach(p, t.second);
    }
    while (!pending.empty()) {
      Delta delta = std::move(pending.front()); pending.pop_front();
      if (!expanded[delta.id]) {
        expanded[delta.id] = true;
        ProductNode n = pn[delta.id];
        for (auto eid : G.outgoing(n.node)) {
          ++stats.edgesExamined;
          const auto &e = G.edge(eid);
          if (!e.enabled || e.objects.empty() || (q.edgeFilter && !q.edgeFilter(e))) continue;
          if (!edgeMaskReady[eid]) {
            edgeMasks[eid] = objectMask(e.objects);
            edgeMaskReady[eid] = true;
          }
          const auto &edgeMask = edgeMasks[eid];
          if (e.kind == FlowKind::Thread && !q.includeThreadEdges) {
            unite(omittedThreads[delta.id], edgeMask);
            continue;
          }
          for (const auto &t : effects(e.to, n.state)) {
            auto label = intersect(edgeMask, t.second);
            if (label == emptyMask) continue;
            transitions[delta.id].push_back({e.to, t.first, eid, std::move(label)});
          }
        }
      }
      if (omittedThreads[delta.id] != emptyMask)
        unite(incomplete, intersect(delta.objects, omittedThreads[delta.id]));
      for (std::size_t i = 0; i < transitions[delta.id].size(); ++i) {
        const auto &transition = transitions[delta.id][i];
        auto arriving = intersect(delta.objects, transition.objects);
        if (arriving == emptyMask) continue;
        ID id = transition.edge;
        if (id == InvalidID) {
          const auto node = transition.node;
          const auto state = transition.state;
          const auto original = transition.original;
          ID target = product(node, state);
          id = pe.size();
          pe.push_back({delta.id, target, original, static_cast<ID>(i)});
          transitions[delta.id][i].edge = id;
          pout[delta.id].push_back(id);
          const auto kind = G.edge(original).kind;
          hasContext |= kind == FlowKind::Call || kind == FlowKind::Return;
        }
        reach(pe[id].to, arriving);
      }
    }
  } catch (const Budget &budget) {
    limited = true;
    result.completion.stop(budget.reason, budget.limit, budget.observed);
  }
  stats.productStates = pn.size(); stats.productEdges = pe.size();
  const bool context = hasContext && q.context != ContextMode::Insensitive;
  if (request.retainWitnesses && (q.contextLimit || !context)) {
    result.Witnesses = std::make_shared<ObjectWitnessData>();
    result.Witnesses->maxEdges = q.maxWitnessEdges;
  }
  auto proof = [&](ID parent, FlowEdgeID edge, ID product) -> ID {
    if (!result.Witnesses) return InvalidID;
    auto &proofs = result.Witnesses->proofs;
    if (proofs.size() >= InvalidID)
      throw std::length_error("UseTraceSSA: witness provenance too large");
    ID id = proofs.size();
    proofs.push_back({parent, edge, pn[product].node, pn[product].state});
    return id;
  };
  auto accept = [&](FlowNodeID sink, const Delta &delta) {
    auto &accepted = foundAt.emplace(sink, emptyMask).first->second;
    if (result.Witnesses) {
      // A delta's objects all follow its parent's proof. Only acceptance needs
      // a mask: the immutable parent chain is already compatible with every bit.
      auto fresh = difference(delta.objects, accepted);
      if (fresh != emptyMask)
        result.Witnesses->accepted[sink].push_back({std::move(fresh), delta.proof});
    }
    unite(foundObjects, delta.objects);
    unite(accepted, delta.objects);
  };
  auto exportResult = [&] {
    auto unknown = difference(limited ? allMask : incomplete, foundObjects);
    result.found = *foundObjects;
    result.unknown = *unknown;
    result.notFound = *difference(difference(allMask, foundObjects), unknown);
    result.completion.modelComplete = G.complete() && incomplete == emptyMask;
    result.complete = result.completion.complete();
    for (const auto &sink : foundAt) result.foundAt.emplace(sink.first, *sink.second);
    stats.foundObjects = result.found.count();
    stats.notFoundObjects = result.notFound.count();
    stats.unknownObjects = result.unknown.count();
  };

  if (q.contextLimit && q.context != ContextMode::Insensitive) {
    using detail::ContextState;
    std::unordered_map<ContextState, ID, detail::ContextStateHash> known;
    std::vector<ContextState> states;
    std::vector<Mask> visited;
    std::deque<Delta> queue;
    auto push = [&](ContextState state, const Mask &objects,
                    ID parent = InvalidID, FlowEdgeID edge = InvalidFlowID) {
      if (objects == emptyMask) return;
      auto found = known.find(state);
      ID id;
      if (found == known.end()) {
        bound(states.size(), q.maxSummaryPairs, SearchStopReason::SummaryPairs);
        if (states.size() >= InvalidID)
          throw std::length_error("UseTraceSSA: context state identifier overflow");
        id = states.size();
        known.emplace(state, id);
        states.push_back(std::move(state));
        visited.push_back(emptyMask);
      } else {
        id = found->second;
      }
      auto delta = difference(objects, visited[id]);
      if (delta == emptyMask) return;
      unite(visited[id], delta);
      ++stats.nonemptyDeltas;
      queue.push_back({id, std::move(delta), false, proof(parent, edge, states[id].product)});
    };
    try {
      for (const auto &root : roots)
        push({root.first, {}, false, false}, root.second);
      while (!queue.empty()) {
        Delta delta = std::move(queue.front()); queue.pop_front();
        ContextState current = states[delta.id];
        ProductNode point = pn[current.product];
        if (sinks[point.node] && accepts[point.state] &&
            (!q.requireNonEmpty || current.positive) &&
            (q.context != ContextMode::Balanced || current.calls.empty())) {
          accept(point.node, delta);
        }
        for (ID edgeID : pout[current.product]) {
          const ProductEdge &step = pe[edgeID];
          auto permitted = intersect(delta.objects, productMask(step));
          if (permitted == emptyMask) continue;
          const FlowEdge &edge = G.edge(step.original);
          ContextState next = current;
          next.product = step.to;
          next.positive = true;
          if (edge.kind == FlowKind::Call) {
            if (next.calls.size() >= *q.contextLimit) {
              if (!next.calls.empty()) next.calls.erase(next.calls.begin());
              next.truncated = true;
            }
            next.calls.push_back(edge.callSite);
          } else if (edge.kind == FlowKind::Return) {
            if (next.calls.empty()) {
              if (q.context == ContextMode::Balanced && !next.truncated) continue;
            } else {
              if (next.calls.back() != edge.callSite) continue;
              next.calls.pop_back();
            }
          }
          push(std::move(next), permitted, delta.proof, step.original);
        }
      }
    } catch (const Budget &budget) {
      limited = true;
      result.completion.stop(budget.reason, budget.limit, budget.observed);
    }
    stats.summaryPairs = states.size();
    exportResult();
    result.message = limited ? "bounded-context query budget exhausted" :
        incomplete != emptyMask ? "incomplete model or omitted thread edges" :
                           "bounded-context mask tabulation complete";
    return result;
  }

  // D ::= epsilon | local | D D | call_c D return_c. Sequential composition
  // intersects; alternative derivations union for the SAME relation pair.
  // Separate positive masks retain nonempty cycles even when epsilon exists.
  std::vector<Summary> summaries;
  std::vector<std::vector<ID>> rout(pn.size()), rin(pn.size());
  if (context) {
    std::unordered_map<std::uint64_t, ID> relation;
    std::deque<Delta> queue;
    std::vector<std::vector<ID>> callsIn(pn.size());
    std::vector<std::unordered_map<CallSiteID, std::vector<ID>>> returnsBySite(pn.size());
    auto insert = [&](ID from, ID to, const Mask &objects, bool positive) {
      if (objects == emptyMask) return;
      auto k = key(from, to);
      auto it = relation.find(k);
      if (it == relation.end()) {
        bound(summaries.size(), q.maxSummaryPairs, SearchStopReason::SummaryPairs);
        ID id = summaries.size();
        it = relation.emplace(k, id).first;
        summaries.push_back({from, to, emptyMask, emptyMask});
        rout[from].push_back(id); rin[to].push_back(id);
      }
      ID id = it->second;
      auto update = [&](Mask &old, bool pos) {
        auto delta = difference(objects, old);
        if (delta == emptyMask) return;
        unite(old, delta); ++stats.nonemptyDeltas;
        queue.push_back({id, std::move(delta), pos});
      };
      update(summaries[id].reachable, false);
      if (positive) update(summaries[id].positive, true);
    };
    try {
      for (ID i = 0; i < pn.size(); ++i) insert(i, i, allMask, false);
      for (ID i = 0; i < pe.size(); ++i) {
        const auto &e = pe[i]; auto kind = G.edge(e.original).kind;
        if (kind == FlowKind::Call) callsIn[e.to].push_back(i);
        else if (kind == FlowKind::Return)
          returnsBySite[e.from][G.edge(e.original).callSite].push_back(i);
        else insert(e.from, e.to, productMask(e), true);
      }
      while (!queue.empty()) {
        Delta d = std::move(queue.front()); queue.pop_front();
        // insert() can reallocate both summaries and adjacency vectors.
        ID from = summaries[d.id].from, to = summaries[d.id].to;
        std::size_t preds = rin[from].size(), succs = rout[to].size();
        for (std::size_t i = 0; i < preds; ++i) {
          ID predecessor = rin[from][i], source = summaries[predecessor].from;
          auto reachable = intersect(summaries[predecessor].reachable, d.objects);
          if (d.positive) {
            insert(source, to, reachable, true);
          } else {
            auto positive = intersect(summaries[predecessor].positive, d.objects);
            insert(source, to, reachable, false);
            insert(source, to, positive, true);
          }
        }
        for (std::size_t i = 0; i < succs; ++i) {
          ID successor = rout[to][i], target = summaries[successor].to;
          auto reachable = intersect(d.objects, summaries[successor].reachable);
          if (d.positive) {
            insert(from, target, reachable, true);
          } else {
            auto positive = intersect(d.objects, summaries[successor].positive);
            insert(from, target, reachable, false);
            insert(from, target, positive, true);
          }
        }
        if (!d.positive) for (auto c : callsIn[from]) {
          auto site = G.edge(pe[c].original).callSite;
          auto matches = returnsBySite[to].find(site);
          if (matches == returnsBySite[to].end()) continue;
          for (auto r : matches->second) {
            insert(pe[c].from, pe[r].to,
                   intersect(intersect(productMask(pe[c]), d.objects), productMask(pe[r])),
                   true);
          }
        }
      }
    } catch (const Budget &budget) {
      limited = true;
      result.completion.stop(budget.reason, budget.limit, budget.observed);
    }
  }
  stats.summaryPairs = summaries.size();

  // Existing realizable quotient: unmatched returns precede unmatched calls.
  // Delta masks belong to logical (product, phase, positive) states; no object
  // creates a separate search state or a per-object witness recipe.
  std::vector<Mask> visited(pn.size() * 4, emptyMask);
  std::deque<Delta> queue;
  auto push = [&](ID state, const Mask &objects,
                  ID parent = InvalidID, FlowEdgeID edge = InvalidFlowID) {
    auto delta = difference(objects, visited[state]);
    if (delta == emptyMask) return;
    unite(visited[state], delta); ++stats.nonemptyDeltas;
    queue.push_back({state, std::move(delta), false, proof(parent, edge, state / 4)});
  };
  for (const auto &root : roots) push(root.first * 4, root.second);
  while (!queue.empty()) {
    Delta d = std::move(queue.front()); queue.pop_front();
    ID p = d.id / 4; unsigned phase = (d.id % 4) / 2; bool positive = d.id % 2;
    if (sinks[pn[p].node] && accepts[pn[p].state] && (!q.requireNonEmpty || positive)) {
      accept(pn[p].node, d);
    }
    auto step = [&](ID target, unsigned nextPhase, bool nonempty, const Mask &mask,
                    FlowEdgeID edge = InvalidFlowID) {
      push(target * 4 + nextPhase * 2 + unsigned(positive || nonempty),
           intersect(d.objects, mask), d.proof, edge);
    };
    if (context) for (auto id : rout[p]) {
      const auto &s = summaries[id];
      step(s.to, phase, false, s.reachable);
      step(s.to, phase, true, s.positive);
    }
    for (auto id : pout[p]) {
      const auto &e = pe[id]; auto kind = G.edge(e.original).kind;
      if (!context || (kind != FlowKind::Call && kind != FlowKind::Return))
        step(e.to, phase, true, productMask(e), e.original);
      else if (q.context == ContextMode::Realizable) {
        if (kind == FlowKind::Call) step(e.to, 1, true, productMask(e), e.original);
        else if (kind == FlowKind::Return && !phase)
          step(e.to, 0, true, productMask(e), e.original);
      }
    }
  }
  exportResult();
  result.message = limited ? "symbolic query budget exhausted; remaining objects unknown" :
      incomplete != emptyMask ? "incomplete model or omitted thread edges" :
                            "symbolic same-object reachability complete";
  return result;
}
} // namespace usetracessa
} // namespace lotus
