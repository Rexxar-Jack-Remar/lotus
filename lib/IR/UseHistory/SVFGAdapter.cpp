#include "IR/UseHistory/SVFGAdapter.h"
#include <algorithm>
#include <set>
#include <stdexcept>

namespace lotus {
namespace usehistory {
Port Port::definition(FunctionID f, ValueID v) {
  Port p; p.function = f; p.value = v; return p;
}
Port Port::afterUse(FunctionID f, SiteID s, ValueID v) {
  Port p; p.kind = PortKind::AfterUse; p.function = f; p.site = s; p.value = v; return p;
}
Port Port::flowNode(FlowNodeID id) {
  Port p; p.kind = PortKind::FlowNode; p.node = id; return p;
}
FlowNodeID Port::resolve(const FlowGraph &g) const {
  switch (kind) {
  case PortKind::Definition: return g.definition(function, value);
  case PortKind::BeforeUse: return g.before(function, site, value);
  case PortKind::AfterUse: return g.after(function, site, value);
  case PortKind::Version: return g.version(function, version);
  case PortKind::FlowNode: g.node(node); return node;
  }
  throw std::invalid_argument("UseHistory: invalid port kind");
}
SVFGImportResult SVFGAdapter::append(FlowGraph &graph, const SVFGSnapshot &view) {
  SVFGImportResult result;
  // Resolve and validate ALL records before mutating the graph. Do not copy
  // an entire graph merely to achieve transactional validation.
  std::map<NativeID, Port> outputs;
  for (const auto &n : view.nodes) {
    if (n.id == NoNativeID || !outputs.emplace(n.id, n.output).second)
      throw std::invalid_argument("UseHistory: duplicate/invalid SVFG node ID");
    result.nodes.emplace(n.id, n.output.resolve(graph));
  }
  std::map<NativeID, FlowEdge> pending;
  for (const auto &e : view.edges) {
    if (e.id == NoNativeID || pending.count(e.id))
      throw std::invalid_argument("UseHistory: duplicate/invalid SVFG edge ID");
    if (!result.nodes.count(e.from) || !result.nodes.count(e.to))
      throw std::invalid_argument("UseHistory: SVFG edge references an unmapped node");
    FlowNodeID from = result.nodes.at(e.from), to = result.nodes.at(e.to);
    if (e.consumption) {
      const Port &use = *e.consumption;
      if (use.kind != PortKind::AfterUse)
        throw std::invalid_argument("UseHistory: a consumer must bind an AFTER-use port");
      from = use.resolve(graph);
      // Ensure the channel consumed really belongs to this source definition.
      // Checking only reachability would accept a different definition's flow.
      auto def = graph.definition(use.function, use.value);
      const Port &origin = outputs.at(e.from);
      if (origin.resolve(graph) != def)
        throw std::invalid_argument("UseHistory: consumer uses a different source channel");
    } else if (!e.sourceIsBoundary) {
      throw std::invalid_argument("UseHistory: missing consumer binding; raw def-use bypass forbidden");
    }
    if ((e.kind == FlowKind::Call || e.kind == FlowKind::Return) &&
        e.callSite == NoNativeID)
      throw std::invalid_argument("UseHistory: missing SVFG call-site context");
    FlowEdge edge;
    edge.from = from; edge.to = to; edge.kind = e.kind; edge.callSite = e.callSite;
    edge.objects = e.objects; edge.native = e.id; edge.nativeKind = e.nativeKind;
    edge.guard = e.guard;
    pending.emplace(e.id, std::move(edge));
  }
  for (const auto &p : pending) result.edges.emplace(p.first, graph.addEdge(p.second));
  for (const auto &issue : view.issues) graph.addIssue(issue);
  // Native node IDs need not be injective onto history ports (e.g. actual
  // parameter pseudo-nodes); result.nodes retains this many-to-one provenance.
  return result;
}
} // namespace usehistory
} // namespace lotus
