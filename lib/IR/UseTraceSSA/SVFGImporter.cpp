#include "IR/UseTraceSSA/SVFGImporter.h"
#include <algorithm>
#include <set>
#include <stdexcept>

namespace lotus {
namespace usetracessa {

// ---------------------------------------------------------------------------
// Port resolution
// ---------------------------------------------------------------------------

Port Port::definition(FunctionID f, ValueID v) {
  Port p; p.function = f; p.value = v; return p;
}
Port Port::afterUse(FunctionID f, SiteID s, ValueID v) {
  Port p; p.kind = PortKind::AfterUse; p.function = f; p.site = s; p.value = v; return p;
}
Port Port::flowNode(FlowNodeID id) {
  Port p; p.kind = PortKind::FlowNode; p.node = id; return p;
}
FlowNodeID Port::resolve(const TraceFlowGraph &g) const {
  switch (kind) {
  case PortKind::Definition: return g.definition(function, value);
  case PortKind::BeforeUse: return g.before(function, site, value);
  case PortKind::AfterUse: return g.after(function, site, value);
  case PortKind::Version: return g.version(function, version);
  case PortKind::FlowNode: g.node(node); return node;
  }
  throw std::invalid_argument("UseTraceSSA: invalid port kind");
}

// ---------------------------------------------------------------------------
// Snapshot import (formerly SVFGAdapter::append)
// ---------------------------------------------------------------------------

SVFGImportResult SVFGImporter::append(TraceFlowGraph &graph, const SVFGSnapshot &view) {
  SVFGImportResult result;
  std::map<NativeID, Port> outputs;
  for (const auto &n : view.nodes) {
    if (n.id == NoNativeID || !outputs.emplace(n.id, n.output).second)
      throw std::invalid_argument("UseTraceSSA: duplicate/invalid SVFG node ID");
    result.nodes.emplace(n.id, n.output.resolve(graph));
  }
  std::map<NativeID, FlowEdge> pending;
  for (const auto &e : view.edges) {
    if (e.id == NoNativeID || pending.count(e.id))
      throw std::invalid_argument("UseTraceSSA: duplicate/invalid SVFG edge ID");
    if (!result.nodes.count(e.from) || !result.nodes.count(e.to))
      throw std::invalid_argument("UseTraceSSA: SVFG edge references an unmapped node");
    FlowNodeID from = result.nodes.at(e.from), to = result.nodes.at(e.to);
    if (e.consumption) {
      const Port &use = *e.consumption;
      if (use.kind != PortKind::AfterUse)
        throw std::invalid_argument("UseTraceSSA: a consumer must bind an AFTER-use port");
      from = use.resolve(graph);
      auto def = graph.definition(use.function, use.value);
      const Port &origin = outputs.at(e.from);
      if (origin.resolve(graph) != def)
        throw std::invalid_argument("UseTraceSSA: consumer uses a different source channel");
    } else if (!e.sourceIsBoundary) {
      throw std::invalid_argument("UseTraceSSA: missing consumer binding; raw def-use bypass forbidden");
    }
    if ((e.kind == FlowKind::Call || e.kind == FlowKind::Return) &&
        e.callSite == NoNativeID)
      throw std::invalid_argument("UseTraceSSA: missing SVFG call-site context");
    FlowEdge edge;
    edge.from = from; edge.to = to; edge.kind = e.kind; edge.callSite = e.callSite;
    edge.objects = e.objects; edge.native = e.id; edge.nativeKind = e.nativeKind;
    edge.guard = e.guard;
    pending.emplace(e.id, std::move(edge));
  }
  for (const auto &p : pending) result.edges.emplace(p.first, graph.addEdge(p.second));
  for (const auto &issue : view.issues) graph.addIssue(issue);
  return result;
}

// ---------------------------------------------------------------------------
// Full history construction (formerly SVFGHistoryBuilder::build)
// ---------------------------------------------------------------------------

SVFGHistoryResult SVFGImporter::build(const SVFGConstructionInput &in) {
  SVFGHistoryResult out;
  std::map<FunctionID, const FunctionLayout *> functions;
  std::map<NativeID, const LocatedSVFGNode *> nodes;
  std::set<NativeID> edgeIDs;
  for (const auto &f : in.functions)
    if (f.id == InvalidID || !functions.emplace(f.id, &f).second)
      throw std::invalid_argument("UseTraceSSA: duplicate/invalid function layout");
  for (const auto &n : in.nodes) {
    if (n.id == NoNativeID || !nodes.emplace(n.id, &n).second || !functions.count(n.function))
      throw std::invalid_argument("UseTraceSSA: invalid located SVFG node");
    if (n.definitionSite != InvalidID &&
        n.definitionSite >= functions.at(n.function)->control.operations().size())
      throw std::invalid_argument("UseTraceSSA: definition site outside function");
  }
  std::map<NativeID, SiteID> consumers;
  for (const auto &e : in.edges) {
    if (e.id == NoNativeID || !edgeIDs.insert(e.id).second ||
        !nodes.count(e.from) || !nodes.count(e.to))
      throw std::invalid_argument("UseTraceSSA: invalid located SVFG edge");
    const auto &from = *nodes.at(e.from), &to = *nodes.at(e.to);
    if (!e.boundary) {
      if (from.function != to.function)
        throw std::invalid_argument("UseTraceSSA: cross-function edge lacks boundary semantics");
      SiteID site = e.consumerSite == InvalidID ? to.definitionSite : e.consumerSite;
      if (site == InvalidID || site >= functions.at(to.function)->control.operations().size())
        throw std::invalid_argument("UseTraceSSA: local transfer has no consumer site");
      consumers[e.id] = site;
    } else if (e.useEvents != Event::None) {
      throw std::invalid_argument("UseTraceSSA: boundary edge cannot invent a local use event");
    }
  }
  for (const auto &pair : functions) {
    const auto &f = *pair.second;
    Program p;
    for (const auto &b : f.control.blocks()) p.addBlock(b.name);
    if (!f.control.blocks().empty()) p.setEntry(f.control.entry());
    for (const auto &e : f.control.edges()) p.addEdge(e.from, e.to, e.label);
    std::map<SiteID, std::vector<ValueID>> definitions, uses;
    std::vector<ValueID> entry;
    for (const auto &np : nodes) {
      const auto &n = *np.second;
      if (n.function != f.id) continue;
      auto value = p.addValue(n.label.empty() ? "svfg:" + std::to_string(n.id) : n.label);
      out.channels.emplace(n.id, value);
      if (n.definitionSite == InvalidID) entry.push_back(value);
      else definitions[n.definitionSite].push_back(value);
    }
    if (p.blocks().empty() && !entry.empty())
      throw std::invalid_argument("UseTraceSSA: entry definitions in an empty layout");
    if (!p.blocks().empty()) p.addOperation(p.entry(), "SVFG live-on-entry", {}, entry);
    for (const auto &e : in.edges)
      if (!e.boundary && nodes.at(e.from)->function == f.id)
        uses[consumers.at(e.id)].push_back(out.channels.at(e.from));
    auto add = [&](SiteID old, BlockID block, EdgeID edge) {
      const auto &label = f.control.operations().at(old).label;
      auto now = edge == InvalidID ? p.addOperation(block, label, uses[old], definitions[old]) :
                                     p.addEdgeOperation(edge, label, uses[old], definitions[old]);
      out.sites[f.id][old] = now;
    };
    for (BlockID b = 0; b < f.control.blocks().size(); ++b)
      for (auto s : f.control.blocks()[b].operations) add(s, b, InvalidID);
    for (EdgeID e = 0; e < f.control.edges().size(); ++e)
      for (auto s : f.control.edges()[e].operations) add(s, InvalidID, e);
    out.graph.addHistory(f.id, f.name, Graph::build(std::move(p)));
  }
  SVFGSnapshot snapshot;
  std::set<NativeID> reachable;
  for (const auto &np : nodes) {
    const auto &n = *np.second;
    auto value = out.channels.at(n.id);
    if (out.graph.layer(n.function).history.definition(value) == InvalidID) continue;
    reachable.insert(n.id);
    Port port = Port::definition(n.function, value);
    snapshot.nodes.push_back({n.id, port});
    out.graph.setNative(port.resolve(out.graph), n.id);
    if (n.definitionEvents != Event::None)
      out.graph.annotate(port.resolve(out.graph), n.definitionEvents, n.certainty);
  }
  for (const auto &e : in.edges) {
    if (!reachable.count(e.from) || !reachable.count(e.to)) continue;
    SVFGEdgeRecord edge;
    edge.id = e.id; edge.from = e.from; edge.to = e.to; edge.kind = e.kind;
    edge.callSite = e.callSite; edge.objects = e.objects; edge.nativeKind = e.nativeKind;
    edge.guard = e.guard; edge.sourceIsBoundary = e.boundary;
    if (!e.boundary) {
      auto function = nodes.at(e.from)->function;
      auto site = out.sites.at(function).at(consumers.at(e.id));
      auto value = out.channels.at(e.from);
      if (!out.graph.layer(function).history.use(site, value)) continue; // proven dead CFG site
      edge.consumption = Port::afterUse(function, site, value);
      if (e.useEvents != Event::None)
        out.graph.annotate(edge.consumption->resolve(out.graph), e.useEvents, e.certainty);
    }
    snapshot.edges.push_back(std::move(edge));
  }
  snapshot.issues = in.issues;
  out.native = SVFGImporter::append(out.graph, snapshot);
  return out;
}
} // namespace usetracessa
} // namespace lotus
