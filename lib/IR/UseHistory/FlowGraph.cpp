#include "IR/UseHistory/FlowGraph.h"
#include <algorithm>
#include <iomanip>
#include <ostream>
#include <sstream>
#include <stdexcept>

namespace lotus {
namespace usehistory {
namespace {
ID checkedID(std::size_t n) {
  if (n >= InvalidID) throw std::length_error("UseHistory: graph too large");
  return static_cast<ID>(n);
}
std::string quoted(const std::string &s) {
  std::ostringstream out;
  out << '"';
  for (unsigned char c : s) {
    switch (c) {
    case '"': out << "\\\""; break;
    case '\\': out << "\\\\"; break;
    case '\n': out << "\\n"; break;
    case '\r': out << "\\r"; break;
    case '\t': out << "\\t"; break;
    default:
      if (c < 32) out << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                      << static_cast<unsigned>(c) << std::dec;
      else out << c;
    }
  }
  out << '"';
  return out.str();
}
const char *kindName(FlowKind kind) {
  switch (kind) {
  case FlowKind::History: return "history";
  case FlowKind::Direct: return "direct";
  case FlowKind::Memory: return "memory";
  case FlowKind::Call: return "call";
  case FlowKind::Return: return "return";
  case FlowKind::Summary: return "summary";
  case FlowKind::Dependence: return "dependence";
  case FlowKind::Thread: return "thread";
  }
  return "invalid";
}
} // namespace
ObjectSet ObjectSet::unknown() { return {}; }
ObjectSet ObjectSet::known(std::vector<ObjectID> objects) {
  std::sort(objects.begin(), objects.end());
  objects.erase(std::unique(objects.begin(), objects.end()), objects.end());
  ObjectSet result;
  result.Unknown = false;
  result.Objects = std::move(objects);
  return result;
}
bool ObjectSet::contains(ObjectID object) const {
  return Unknown || std::binary_search(Objects.begin(), Objects.end(), object);
}
bool ObjectSet::intersects(const ObjectSet &other) const {
  if (empty() || other.empty()) return false;
  if (Unknown || other.Unknown) return true;
  std::size_t i = 0, j = 0;
  while (i < Objects.size() && j < other.Objects.size()) {
    if (Objects[i] == other.Objects[j]) return true;
    if (Objects[i] < other.Objects[j]) ++i; else ++j;
  }
  return false;
}
FlowNodeID FlowGraph::addNode(FlowNode node) {
  node.id = checkedID(Nodes.size());
  Nodes.push_back(std::move(node));
  Out.emplace_back(); In.emplace_back(); ++Revision;
  return Nodes.back().id;
}
FlowEdgeID FlowGraph::addEdge(FlowEdge edge) {
  if (edge.from >= Nodes.size() || edge.to >= Nodes.size())
    throw std::out_of_range("UseHistory: invalid flow edge endpoint");
  if ((edge.kind == FlowKind::Call || edge.kind == FlowKind::Return) &&
      edge.callSite == NoNativeID)
    throw std::invalid_argument("UseHistory: call/return edge needs a call-site ID");
  edge.id = checkedID(Edges.size());
  Edges.push_back(std::move(edge));
  Out[Edges.back().from].push_back(Edges.back().id);
  In[Edges.back().to].push_back(Edges.back().id);
  ++Revision;
  return Edges.back().id;
}
void FlowGraph::addHistory(FunctionID function, std::string name, Graph history) {
  if (function == InvalidID || Layers.count(function))
    throw std::invalid_argument("UseHistory: duplicate/invalid layer ID");
  std::string error;
  if (!history.verify(&error)) throw std::invalid_argument(error);
  HistoryLayer entry;
  entry.function = function; entry.name = std::move(name);
  entry.history = std::move(history);
  for (const Node &n : entry.history.nodes()) {
    FlowNode node;
    node.function = function; node.version = n.id;
    node.label = entry.name + ":" + entry.history.program().values().at(n.value) +
                 "." + std::to_string(n.id);
    if (n.site != InvalidID)
      node.label += " " + entry.history.program().operations().at(n.site).label;
    else if (n.kind == NodeKind::Phi) node.label += " history-phi";
    entry.versions.push_back(addNode(std::move(node)));
  }
  for (const Node &n : entry.history.nodes())
    for (const auto &pred : n.incoming) {
      FlowEdge edge;
      edge.from = entry.versions.at(pred.version); edge.to = entry.versions.at(n.id);
      edge.kind = FlowKind::History; edge.predecessor = pred.predecessor;
      addEdge(std::move(edge));
    }
  Layers.emplace(function, std::move(entry));
  ++Revision;
}
const HistoryLayer &FlowGraph::layer(FunctionID function) const {
  return Layers.at(function);
}
FlowNodeID FlowGraph::version(FunctionID f, VersionID v) const {
  return layer(f).versions.at(v);
}
FlowNodeID FlowGraph::definition(FunctionID f, ValueID v) const {
  auto d = layer(f).history.definition(v);
  if (d == InvalidID) throw std::invalid_argument("UseHistory: unreachable definition port");
  return version(f, d);
}
FlowNodeID FlowGraph::before(FunctionID f, SiteID s, ValueID v) const {
  auto *u = layer(f).history.use(s, v);
  if (!u) throw std::invalid_argument("UseHistory: missing before-use port");
  return version(f, u->before);
}
FlowNodeID FlowGraph::after(FunctionID f, SiteID s, ValueID v) const {
  auto *u = layer(f).history.use(s, v);
  if (!u) throw std::invalid_argument("UseHistory: missing after-use port");
  return version(f, u->after);
}
void FlowGraph::annotate(FlowNodeID id, Event e, Certainty c) {
  auto &n = Nodes.at(id);
  if (n.events != Event::None && n.certainty != c)
    throw std::invalid_argument("UseHistory: split events with different certainties into nodes");
  n.events = n.events | e; n.certainty = c; ++Revision;
}
void FlowGraph::setObject(FlowNodeID id, ObjectID object) {
  Nodes.at(id).object = object; ++Revision;
}
void FlowGraph::setNative(FlowNodeID id, NativeID native) {
  auto &n = Nodes.at(id);
  if (n.native != NoNativeID && n.native != native)
    throw std::invalid_argument("UseHistory: conflicting native node provenance");
  n.native = native; ++Revision;
}
void FlowGraph::disableEdge(FlowEdgeID id) { Edges.at(id).enabled = false; ++Revision; }
void FlowGraph::addIssue(std::string issue) { Issues.push_back(std::move(issue)); ++Revision; }
std::vector<FlowNodeID> FlowGraph::select(Event events) const {
  std::vector<FlowNodeID> result;
  for (const auto &n : Nodes) if (hasEvent(n.events, events)) result.push_back(n.id);
  return result;
}
bool FlowGraph::verify(std::string *error) const {
  auto fail = [&](const char *s) { if (error) *error = s; return false; };
  if (Nodes.size() != Out.size() || Nodes.size() != In.size()) return fail("adjacency size");
  std::vector<unsigned> seenOut(Edges.size()), seenIn(Edges.size());
  for (std::size_t i = 0; i < Nodes.size(); ++i) {
    if (Nodes[i].id != i) return fail("node ID mismatch");
    for (auto e : Out[i]) {
      if (e >= Edges.size() || Edges[e].from != i) return fail("outgoing edge mismatch");
      ++seenOut[e];
    }
    for (auto e : In[i]) {
      if (e >= Edges.size() || Edges[e].to != i) return fail("incoming edge mismatch");
      ++seenIn[e];
    }
  }
  for (std::size_t i = 0; i < Edges.size(); ++i) {
    const auto &e = Edges[i];
    if (e.id != i || seenOut[i] != 1 || seenIn[i] != 1) return fail("edge ID/adjacency mismatch");
    if ((e.kind == FlowKind::Call || e.kind == FlowKind::Return) && e.callSite == NoNativeID)
      return fail("missing call-site ID");
  }
  if (error) error->clear();
  return true;
}
void FlowGraph::printDOT(std::ostream &out) const {
  out << "digraph UseHistory {\n";
  for (const auto &n : Nodes) out << "  n" << n.id << " [label=" << quoted(n.label) << "];\n";
  for (const auto &e : Edges) if (e.enabled)
    out << "  n" << e.from << " -> n" << e.to << " [label="
        << quoted(std::string(kindName(e.kind)) + (e.guard.empty() ? "" : ":" + e.guard))
        << "];\n";
  out << "}\n";
}
void FlowGraph::printJSON(std::ostream &out) const {
  out << "{\"schema\":\"lotus-usehistory-1\",\"complete\":" << (complete() ? "true" : "false")
      << ",\"issues\":[";
  for (std::size_t i = 0; i < Issues.size(); ++i) out << (i ? "," : "") << quoted(Issues[i]);
  out << "],\"nodes\":[";
  for (const auto &n : Nodes) {
    out << (n.id ? "," : "") << "{\"id\":" << n.id << ",\"label\":" << quoted(n.label)
        << ",\"function\":" << n.function << ",\"version\":" << n.version
        << ",\"native\":" << quoted(std::to_string(n.native))
        << ",\"events\":" << static_cast<std::uint32_t>(n.events)
        << ",\"certainty\":" << quoted(n.certainty == Certainty::Must ? "must" : "may")
        << ",\"object\":";
    if (n.object) out << quoted(std::to_string(*n.object)); else out << "null";
    out << "}";
  }
  out << "],\"edges\":[";
  for (const auto &e : Edges) {
    out << (e.id ? "," : "") << "{\"id\":" << e.id << ",\"from\":" << e.from
        << ",\"to\":" << e.to << ",\"kind\":" << quoted(kindName(e.kind))
        << ",\"callsite\":" << quoted(std::to_string(e.callSite))
        << ",\"native\":" << quoted(std::to_string(e.native))
        << ",\"native_kind\":" << e.nativeKind << ",\"predecessor\":" << e.predecessor
        << ",\"enabled\":" << (e.enabled ? "true" : "false")
        << ",\"guard\":" << quoted(e.guard) << ",\"objects\":";
    if (e.objects.isUnknown()) out << "null";
    else {
      out << '[';
      bool first = true;
      for (auto o : e.objects.objects()) { if (!first) out << ','; first = false;
        out << quoted(std::to_string(o)); }
      out << ']';
    }
    out << '}';
  }
  out << "]}\n";
}
} // namespace usehistory
} // namespace lotus
