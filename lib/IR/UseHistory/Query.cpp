#include "IR/UseHistory/Query.h"
#include <algorithm>
#include <deque>
#include <set>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace lotus {
namespace usehistory {
namespace {
struct Budget {};
std::uint64_t key(ID a, ID b) { return (std::uint64_t(a) << 32) | b; }
ID asID(std::size_t n) {
  if (n >= InvalidID) throw std::length_error("UseHistory query identifier overflow");
  return static_cast<ID>(n);
}
void bound(std::size_t n, std::size_t max) { if (max && n >= max) throw Budget{}; }
struct ProductNode { FlowNodeID node; ID state; };
struct ProductEdge { ID from, to; FlowEdgeID original; };
enum class RecipeKind { Epsilon, Edge, Concat, Match };
struct Recipe {
  ID from, to;
  RecipeKind kind;
  ID left = InvalidID, right = InvalidID, middle = InvalidID;
  bool positive = false;
};
struct Step { ID to; ID recipe = InvalidID; ID edge = InvalidID; bool positive = false; };
} // namespace
Automaton Automaton::ordered(std::vector<Event> sequence) {
  Automaton a;
  a.states = asID(sequence.size() + 1); a.accepting = {a.states - 1};
  a.transition = [sequence = std::move(sequence)](ID state, Event event) {
    return state < sequence.size() && hasEvent(event, sequence[state]) ? state + 1 : state;
  };
  return a;
}
Automaton Automaton::doubleFree() {
  Automaton a; a.states = 3; a.accepting = {2};
  a.transition = [](ID state, Event event) -> ID {
    if (hasEvent(event, Event::Allocate)) return 0;
    if (hasEvent(event, Event::Release)) return std::min<ID>(2, state + 1);
    return state;
  };
  return a;
}
Automaton Automaton::useAfterFree() {
  Automaton a; a.states = 3; a.accepting = {2};
  a.transition = [](ID state, Event event) -> ID {
    if (hasEvent(event, Event::Allocate)) return 0;
    if (state == 2) return 2;
    if (hasEvent(event, Event::Release)) return 1;
    if (state == 1 && hasEvent(event, Event::Dereference)) return 2;
    return state;
  };
  return a;
}
QueryResult QueryEngine::run(const Query &q) const {
  QueryResult result;
  if (!q.automaton.states || q.automaton.initial >= q.automaton.states)
    throw std::invalid_argument("UseHistory: invalid automaton initial state");
  std::vector<bool> accepts(q.automaton.states);
  for (ID s : q.automaton.accepting) {
    if (s >= accepts.size()) throw std::invalid_argument("UseHistory: invalid accepting state");
    accepts[s] = true;
  }
  std::vector<bool> sinks(G.nodes().size()), traps(G.nodes().size());
  for (auto n : q.sinks) { G.node(n); sinks[n] = true; }
  for (auto n : q.sources) G.node(n);
  for (auto n : q.traps) { G.node(n); traps[n] = true; }
  auto effects = [&](FlowNodeID id, ID state) {
    const auto &n = G.node(id);
    std::vector<ID> out;
    if (traps[id]) return out;
    if (q.memoryObject && n.object && n.object != q.memoryObject) return out;
    bool blocked = hasEvent(n.events, q.trapEvents);
    if (!blocked) {
      ID next = q.automaton.transition ? q.automaton.transition(state, n.events) : state;
      if (next >= q.automaton.states)
        throw std::invalid_argument("UseHistory: automaton transition out of range");
      out.push_back(next);
    }
    if (n.certainty == Certainty::May &&
        std::find(out.begin(), out.end(), state) == out.end()) out.push_back(state);
    return out;
  };
  std::vector<ProductNode> pn;
  std::vector<ProductEdge> pe;
  std::vector<std::vector<ID>> pout;
  std::unordered_map<std::uint64_t, ID> products;
  std::vector<ID> roots;
  bool incomplete = !G.complete(), limited = false, hasContext = false;
  std::size_t work = 0;
  auto tick = [&] { bound(work++, q.maxWork); };
  auto product = [&](FlowNodeID node, ID state) -> ID {
    auto k = key(node, state);
    auto it = products.find(k);
    if (it != products.end()) return it->second;
    bound(pn.size(), q.maxProductStates);
    ID id = asID(pn.size());
    products.emplace(k, id); pn.push_back({node, state}); pout.emplace_back();
    return id;
  };
  try {
    for (auto src : q.sources)
      for (auto state : effects(src, q.automaton.initial))
        roots.push_back(product(src, state));
    for (std::size_t i = 0; i < pn.size(); ++i) {
      // product() can reallocate pn: do not retain references into it.
      ProductNode n = pn[i];
      for (auto eid : G.outgoing(n.node)) {
        tick();
        const auto &e = G.edge(eid);
        if (!e.enabled || e.objects.empty() || (q.edgeFilter && !q.edgeFilter(e))) continue;
        if (q.memoryObject && !e.objects.contains(*q.memoryObject)) continue;
        if (e.kind == FlowKind::Thread && !q.includeThreadEdges) { incomplete = true; continue; }
        for (auto state : effects(e.to, n.state)) {
          ID target = product(e.to, state), id = asID(pe.size());
          pe.push_back({asID(i), target, eid}); pout[i].push_back(id);
          hasContext |= e.kind == FlowKind::Call || e.kind == FlowKind::Return;
        }
      }
    }
  } catch (const Budget &) { limited = true; }
  result.productStates = pn.size();

  // Sparse Dyck saturation: D ::= epsilon | local | D D | call_c D return_c.
  // Recipes only refer to previously inserted recipes, forming a witness DAG.
  std::vector<Recipe> recipes;
  std::vector<std::vector<ID>> rout(pn.size()), rin(pn.size());
  bool context = hasContext && q.context != ContextMode::Insensitive;
  if (context) {
    std::unordered_map<std::uint64_t, ID> relation;
    std::deque<ID> queue;
    std::vector<std::vector<ID>> callsIn(pn.size()), returnsOut(pn.size());
    auto insert = [&](Recipe r) {
      auto k = key(r.from, r.to);
      auto previous = relation.find(k);
      if (previous != relation.end() &&
          (recipes[previous->second].positive || !r.positive)) return;
      bound(recipes.size(), q.maxSummaryPairs);
      ID id = asID(recipes.size());
      relation[k] = id; recipes.push_back(r);
      rout[r.from].push_back(id); rin[r.to].push_back(id); queue.push_back(id);
    };
    try {
      for (ID i = 0; i < pn.size(); ++i) insert({i, i, RecipeKind::Epsilon});
      for (ID i = 0; i < pe.size(); ++i) {
        const auto &e = pe[i]; auto kind = G.edge(e.original).kind;
        if (kind == FlowKind::Call) callsIn[e.to].push_back(i);
        else if (kind == FlowKind::Return) returnsOut[e.from].push_back(i);
        else insert({e.from, e.to, RecipeKind::Edge, i, InvalidID, InvalidID, true});
      }
      while (!queue.empty()) {
        ID rid = queue.front(); queue.pop_front();
        Recipe r = recipes[rid];
        // Copy adjacency lists: insert() may append and reallocate them.
        auto preds = rin[r.from], succs = rout[r.to];
        for (auto l : preds) {
          tick(); Recipe a = recipes[l];
          insert({a.from, r.to, RecipeKind::Concat, l, rid, InvalidID,
                  a.positive || r.positive});
        }
        for (auto h : succs) {
          tick(); Recipe b = recipes[h];
          insert({r.from, b.to, RecipeKind::Concat, rid, h, InvalidID,
                  r.positive || b.positive});
        }
        for (auto c : callsIn[r.from]) for (auto ret : returnsOut[r.to]) {
          tick();
          if (G.edge(pe[c].original).callSite == G.edge(pe[ret].original).callSite)
            insert({pe[c].from, pe[ret].to, RecipeKind::Match, c, ret, rid, true});
        }
      }
    } catch (const Budget &) { limited = true; }
  }
  result.summaryPairs = recipes.size();

  // Find a path in the quotient. Realizable segments have unmatched returns
  // only before unmatched calls. Balanced summaries can occur in either phase.
  // The positive bit also allows a genuine cycle when source == sink.
  struct Parent { ID previous = InvalidID; Step step{}; };
  const std::size_t states = pn.size() * 4;
  std::vector<bool> visited(states);
  std::vector<Parent> parents(states);
  std::deque<ID> queue;
  auto encode = [&](ID p, unsigned phase, bool positive) {
    return asID(std::size_t(p) * 4 + phase * 2 + unsigned(positive));
  };
  for (ID root : roots) {
    ID state = encode(root, 0, false);
    if (!visited[state]) { visited[state] = true; queue.push_back(state); }
  }
  ID accepted = InvalidID;
  while (!queue.empty()) {
    ID s = queue.front(); queue.pop_front();
    ID p = s / 4; unsigned phase = (s % 4) / 2; bool positive = s % 2;
    if (sinks[pn[p].node] && accepts[pn[p].state] && (!q.requireNonEmpty || positive)) {
      accepted = s; break;
    }
    auto push = [&](Step step, unsigned nextPhase) {
      ID t = encode(step.to, nextPhase, positive || step.positive);
      if (!visited[t]) { visited[t] = true; parents[t] = {s, step}; queue.push_back(t); }
    };
    if (!context) {
      for (auto e : pout[p]) push({pe[e].to, InvalidID, e, true}, phase);
    } else {
      for (auto r : rout[p]) push({recipes[r].to, r, InvalidID, recipes[r].positive}, phase);
      if (q.context == ContextMode::Realizable)
        for (auto eid : pout[p]) {
          const auto &e = pe[eid]; auto kind = G.edge(e.original).kind;
          if (kind == FlowKind::Call) push({e.to, InvalidID, eid, true}, 1);
          else if (kind == FlowKind::Return && phase == 0)
            push({e.to, InvalidID, eid, true}, 0);
        }
      // Local edges are also available directly; this shortens witnesses.
      // The relation retains a positive summary as well as epsilon on cycles.
      for (auto eid : pout[p]) {
        auto kind = G.edge(pe[eid].original).kind;
        if (kind != FlowKind::Call && kind != FlowKind::Return)
          push({pe[eid].to, InvalidID, eid, true}, phase);
      }
    }
  }
  if (accepted == InvalidID) {
    result.status = limited || incomplete ? QueryStatus::Unknown : QueryStatus::NotFound;
    result.message = limited ? "query budget exhausted; absence is not established" :
                     incomplete ? "incomplete model or omitted thread edges" :
                                  "no witness in the supplied graph abstraction";
    return result;
  }
  result.status = QueryStatus::Found;
  result.message = "witness in the supplied graph abstraction; feasibility not proven";
  std::vector<Step> path;
  ID state = accepted;
  while (parents[state].previous != InvalidID) {
    path.push_back(parents[state].step); state = parents[state].previous;
  }
  std::reverse(path.begin(), path.end());
  result.nodes.push_back(pn[state / 4].node);
  result.automatonStates.push_back(pn[state / 4].state);
  auto append = [&](ID edge) {
    if (q.maxWitnessEdges && result.edges.size() >= q.maxWitnessEdges) throw Budget{};
    result.edges.push_back(pe[edge].original);
    result.nodes.push_back(pn[pe[edge].to].node);
    result.automatonStates.push_back(pn[pe[edge].to].state);
  };
  try {
    for (auto step : path) {
      std::vector<std::pair<bool, ID>> stack;
      stack.push_back({step.recipe != InvalidID,
                       step.recipe != InvalidID ? step.recipe : step.edge});
      while (!stack.empty()) {
        auto item = stack.back(); stack.pop_back();
        if (!item.first) { append(item.second); continue; }
        const auto &r = recipes[item.second];
        switch (r.kind) {
        case RecipeKind::Epsilon: break;
        case RecipeKind::Edge: append(r.left); break;
        case RecipeKind::Concat:
          stack.push_back({true, r.right}); stack.push_back({true, r.left}); break;
        case RecipeKind::Match:
          stack.push_back({false, r.right}); stack.push_back({true, r.middle});
          stack.push_back({false, r.left}); break;
        }
      }
    }
  } catch (const Budget &) {
    result.witnessComplete = false;
    result.message += "; witness rendering truncated (existence is established)";
  }
  return result;
}
std::vector<QueryResult> QueryEngine::runBatch(const std::vector<Query> &queries) const {
  std::vector<QueryResult> results;
  for (const auto &q : queries) results.push_back(run(q));
  return results;
}
CoverageResult QueryEngine::allPathsHitTraps(const Query &query) const {
  Query plain = query; plain.traps.clear(); plain.trapEvents = Event::None;
  auto any = run(plain);
  if (any.status == QueryStatus::Unknown) return {CoverageStatus::Unknown, std::move(any)};
  if (any.status == QueryStatus::NotFound)
    return {CoverageStatus::SinkUnreachable, std::move(any)};
  auto untrapped = run(query);
  auto status = untrapped.status == QueryStatus::Found ? CoverageStatus::UntrappedPath :
                untrapped.status == QueryStatus::NotFound ? CoverageStatus::AllPathsTrapped :
                                                         CoverageStatus::Unknown;
  return {status, std::move(untrapped)};
}
std::vector<FlowNodeID> QueryEngine::slice(const std::vector<FlowNodeID> &seeds,
                                        bool backward,
                                        const std::vector<FlowNodeID> &traps) const {
  std::vector<bool> visited(G.nodes().size()), blocked(G.nodes().size());
  for (auto n : traps) { G.node(n); blocked[n] = true; }
  std::deque<FlowNodeID> queue;
  for (auto n : seeds) { G.node(n); if (!blocked[n] && !visited[n]) {
    visited[n] = true; queue.push_back(n);
  }}
  while (!queue.empty()) {
    auto n = queue.front(); queue.pop_front();
    for (auto id : backward ? G.incoming(n) : G.outgoing(n)) {
      const auto &e = G.edge(id);
      auto target = backward ? e.from : e.to;
      if (e.enabled && !e.objects.empty() && !blocked[target] && !visited[target]) {
        visited[target] = true; queue.push_back(target);
      }
    }
  }
  std::vector<FlowNodeID> result;
  for (ID i = 0; i < visited.size(); ++i) if (visited[i]) result.push_back(i);
  return result;
}
} // namespace usehistory
} // namespace lotus
