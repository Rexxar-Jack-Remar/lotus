#include "IR/UseTraceSSA/ResourceHistory.h"
#include <set>
#include <stdexcept>

namespace lotus {
namespace usetracessa {
FlowNodeID ResourceLayer::before(const TraceFlowGraph &g, SiteID site, ObjectID object) const {
  return g.before(id, sites.at(site), values.at(object));
}
FlowNodeID ResourceLayer::after(const TraceFlowGraph &g, SiteID site, ObjectID object) const {
  return g.after(id, sites.at(site), values.at(object));
}
FlowNodeID ResourceLayer::entry(const TraceFlowGraph &g, ObjectID object) const {
  return g.definition(id, values.at(object));
}
ResourceLayer ResourceHistoryBuilder::append(TraceFlowGraph &graph, FunctionID id, std::string name,
                                            const Program &cfg,
                                            const std::vector<ResourceAccess> &accesses,
                                            const std::vector<ObjectID> &universe) {
  ResourceLayer result; result.id = id;
  std::set<ObjectID> objects(universe.begin(), universe.end());
  for (const auto &access : accesses) {
    if (access.site >= cfg.operations().size())
      throw std::invalid_argument("UseTraceSSA: resource access has invalid site");
    if (access.objects.isUnknown()) { objects.insert(UnknownResource); }
    else for (auto o : access.objects.objects()) objects.insert(o);
  }
  Program p;
  for (const auto &b : cfg.blocks()) p.addBlock(b.name);
  if (!cfg.blocks().empty()) p.setEntry(cfg.entry());
  for (const auto &e : cfg.edges()) p.addEdge(e.from, e.to, e.label);
  std::vector<ValueID> all;
  for (auto object : objects) {
    auto value = p.addValue("object:" + std::to_string(object));
    result.values.emplace(object, value); all.push_back(value);
  }
  if (!cfg.blocks().empty()) p.addOperation(p.entry(), "object history entry", {}, all);
  else if (!objects.empty()) throw std::invalid_argument("UseTraceSSA: objects in empty CFG");
  struct Effect { Event events = Event::None; Certainty certainty = Certainty::Must; };
  std::map<SiteID, std::map<ObjectID, Effect>> effects;
  for (const auto &a : accesses)
    for (auto o : objects) if (a.objects.contains(o)) {
      auto &effect = effects[a.site][o];
      effect.events = effect.events | a.events;
      if (a.certainty == Certainty::May || a.objects.isUnknown()) effect.certainty = Certainty::May;
    }
  auto addSite = [&](SiteID old, BlockID block, EdgeID edge) {
    std::vector<ValueID> uses;
    for (const auto &effect : effects[old]) uses.push_back(result.values.at(effect.first));
    SiteID now = edge == InvalidID ?
        p.addOperation(block, cfg.operations().at(old).label, uses) :
        p.addEdgeOperation(edge, cfg.operations().at(old).label, uses);
    result.sites.emplace(old, now);
  };
  std::vector<bool> hasSuccessor(cfg.blocks().size());
  for (const auto &e : cfg.edges()) hasSuccessor[e.from] = true;
  for (BlockID b = 0; b < cfg.blocks().size(); ++b) {
    for (auto s : cfg.blocks()[b].operations) addSite(s, b, InvalidID);
    if (!hasSuccessor[b])
      result.exits.emplace(b, p.addOperation(b, "object history exit", all));
  }
  for (EdgeID e = 0; e < cfg.edges().size(); ++e)
    for (auto s : cfg.edges()[e].operations) addSite(s, InvalidID, e);
  Graph history = Graph::build(std::move(p));
  graph.addHistory(id, std::move(name), std::move(history));
  std::vector<ObjectID> objectForValue(result.values.size());
  for (const auto &entry : result.values) objectForValue[entry.second] = entry.first;
  const auto &layer = graph.layer(id);
  for (const auto &n : layer.history.nodes())
    graph.setObject(layer.versions[n.id], objectForValue.at(n.value));
  for (const auto &site : effects) for (const auto &effect : site.second) {
    SiteID now = result.sites.at(site.first);
    ValueID value = result.values.at(effect.first);
    if (graph.layer(id).history.use(now, value))
      graph.annotate(graph.after(id, now, value), effect.second.events, effect.second.certainty);
  }
  for (const auto &exit : result.exits) for (auto value : all)
    if (graph.layer(id).history.use(exit.second, value))
      graph.annotate(graph.after(id, exit.second, value), Event::Exit);
  // Unknown aliasing is a conservative TOP within this universe, not inherently
  // a dropped edge. It does not alone make the graph incomplete.
  return result;
}
void ResourceHistoryBuilder::connectCall(TraceFlowGraph &g, const ResourceLayer &caller,
                                         SiteID site, CallSiteID callSite,
                                         const std::vector<ResourceLayer> &targets,
                                         bool completeTargets) {
  if (callSite == NoNativeID) throw std::invalid_argument("UseTraceSSA: invalid call-site ID");
  if (targets.empty() && completeTargets)
    throw std::invalid_argument("UseTraceSSA: a complete call needs at least one target");
  std::vector<FlowEdgeID> disable;
  std::vector<FlowEdge> pending;
  for (const auto &object : caller.values) {
    FlowNodeID before = caller.before(g, site, object.first);
    FlowNodeID after = caller.after(g, site, object.first);
    if (g.node(after).events != Event::None)
      throw std::invalid_argument("UseTraceSSA: call-splice site must have no semantic event");
    if (completeTargets) {
      for (auto eid : g.outgoing(before)) {
        const auto &e = g.edge(eid);
        if (e.kind == FlowKind::History && e.to == after && e.enabled) disable.push_back(eid);
      }
    }
    for (const auto &target : targets) {
      if (!target.values.count(object.first))
        throw std::invalid_argument("UseTraceSSA: inconsistent module-wide object universe");
      FlowEdge call;
      call.from = before; call.to = target.entry(g, object.first);
      call.kind = FlowKind::Call; call.callSite = callSite;
      call.objects = ObjectSet::known({object.first}); pending.push_back(call);
      for (const auto &exit : target.exits) {
        auto value = target.values.at(object.first);
        if (!g.layer(target.id).history.use(exit.second, value)) continue;
        FlowEdge ret;
        ret.from = g.after(target.id, exit.second, value); ret.to = after;
        ret.kind = FlowKind::Return; ret.callSite = callSite;
        ret.objects = ObjectSet::known({object.first}); pending.push_back(ret);
      }
    }
  }
  for (auto eid : disable) g.disableEdge(eid);
  for (const auto &edge : pending) g.addEdge(edge);
  if (!completeTargets) g.addIssue("unresolved resource-call target or external effect");
}
} // namespace usetracessa
} // namespace lotus
