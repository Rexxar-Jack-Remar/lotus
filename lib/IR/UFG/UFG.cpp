#include "IR/UFG/UFG.h"
#include "IR/UFG/Search.h"

#include <algorithm>
#include <sstream>
#include <stdexcept>

namespace lotus {
namespace ufg {
using namespace usetracessa;
namespace {
bool resource(Event events) {
  return hasEvent(events, Event::Allocate | Event::Release |
                          Event::Dereference | Event::Open | Event::Close);
}
} // namespace

UFGGraph::UFGGraph(const TraceFlowGraph &source, std::vector<ObjectID> universe)
    : Source(source) {
  std::string error;
  if (!source.verify(&error))
    throw std::invalid_argument("UFG: invalid source graph: " + error);
  Objects = source.resourceCandidates();
  Objects.insert(Objects.end(), universe.begin(), universe.end());
  // An unguarded resource effect has an anonymous resource candidate.
  for (const auto &n : source.nodes()) if (resource(n.events)) {
    Objects.push_back(UnknownResource);
    break;
  }
  std::sort(Objects.begin(), Objects.end());
  Objects.erase(std::unique(Objects.begin(), Objects.end()), Objects.end());
  detail::expandedNodeCount(source.nodes().size(), Objects.size());
  for (ObjectID object : Objects) {
    Lane.emplace(object, OriginalNodes.size());
    for (const FlowNode &original : source.nodes()) {
      FlowNode copy = original;
      auto effective = source.effectiveEvent(original.id, object);
      copy.label += " @object:" + std::to_string(object);
      copy.events = effective.events;
      copy.certainty = effective.certainty;
      copy.effects.clear();
      Expanded.addNode(std::move(copy));
      OriginalNodes.push_back(original.id);
      NodeObjects.push_back(object);
    }
    for (const FlowEdge &original : source.edges()) {
      if (!original.objects.contains(object)) continue;
      FlowEdge copy = original;
      copy.from = node(object, original.from);
      copy.to = node(object, original.to);
      copy.objects = ObjectSet::known({object});
      FlowEdgeID id = Expanded.addEdge(std::move(copy));
      if (!original.enabled) Expanded.disableEdge(id);
    }
  }
  for (const auto &issue : source.issues()) Expanded.addIssue(issue);
}

Statistics UFGGraph::statistics() const {
  return {Objects.size(), Expanded.nodes().size(), Expanded.edges().size()};
}

FlowNodeID UFGGraph::node(ObjectID object, FlowNodeID original) const {
  Source.node(original);
  return static_cast<FlowNodeID>(Lane.at(object) + original);
}

FlowNodeID UFGGraph::originalNode(FlowNodeID expanded) const {
  return OriginalNodes.at(expanded);
}

ObjectID UFGGraph::objectOf(FlowNodeID expanded) const {
  return NodeObjects.at(expanded);
}

QueryResult UFGGraph::runObject(Query query, ObjectID object) const {
  return SearchEngine(*this).run(std::move(query), object);
}

void UFGGraph::printDOT(std::ostream &out) const {
  std::ostringstream buffer;
  Expanded.printDOT(buffer);
  std::string dot = buffer.str();
  dot.replace(0, std::string("digraph UseTraceSSA").size(), "digraph UFG");
  out << dot;
}

void UFGGraph::printJSON(std::ostream &out) const {
  out << "{\"schema\":\"lotus-ufg-1\",\"objects\":[";
  for (std::size_t i = 0; i < Objects.size(); ++i)
    out << (i ? "," : "") << '"' << Objects[i] << '"';
  out << "],\"graph\":";
  Expanded.printJSON(out);
  out << "}\n";
}

} // namespace ufg
} // namespace lotus
