#include "IR/UseTraceSSA/SVFGHistoryBuilder.h"
#include <set>
#include <stdexcept>

namespace lotus {
namespace usetracessa {
SVFGHistoryResult SVFGHistoryBuilder::build(const SVFGConstructionInput &in) {
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
  out.native = SVFGAdapter::append(out.graph, snapshot);
  return out;
}
} // namespace usetracessa
} // namespace lotus
