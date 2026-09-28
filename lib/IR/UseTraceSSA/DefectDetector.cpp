#include "IR/UseTraceSSA/DefectDetector.h"

#include <algorithm>
#include <deque>
#include <stdexcept>

namespace {
using namespace lotus::usetracessa;

bool canUseResourceReachability(const TraceFlowGraph &graph) {
  for (const auto &node : graph.nodes()) {
    if (hasEvent(node.events, Event::Release | Event::Dereference |
                                   Event::Allocate) && !node.object)
      return false;
    if (node.object && hasEvent(node.events, Event::Allocate) &&
        hasEvent(node.events, Event::Release | Event::Dereference))
      return false;
    if (node.object && hasEvent(node.events, Event::Release) &&
        hasEvent(node.events, Event::Dereference))
      return false;
  }
  for (const auto &edge : graph.edges()) {
    if (!edge.enabled) continue;
    const auto &from = graph.node(edge.from), &to = graph.node(edge.to);
    if (!from.object && !to.object) continue;
    if (!from.object || !to.object || from.object != to.object ||
        edge.kind != FlowKind::History)
      return false;
  }
  return true;
}

struct ResourceSearch {
  std::vector<QueryResult> findings;
  bool exhausted = false;
};

ResourceSearch searchResources(const TraceFlowGraph &graph, DefectKind kind,
                               bool firstOnly) {
  ResourceSearch search;
  std::vector<bool> visited(graph.nodes().size());
  std::vector<bool> reported(graph.nodes().size());
  std::vector<FlowNodeID> parent(graph.nodes().size(), InvalidID);
  std::vector<FlowEdgeID> incoming(graph.nodes().size(), InvalidID);
  std::deque<FlowNodeID> queue;
  for (const auto &node : graph.nodes()) {
    if (!node.object || !hasEvent(node.events, Event::Release)) continue;
    visited[node.id] = true;
    queue.push_back(node.id);
  }
  std::size_t examined = 0;
  constexpr std::size_t MaxWork = 5000000;
  while (!queue.empty()) {
    FlowNodeID current = queue.front();
    queue.pop_front();
    const auto object = *graph.node(current).object;
    for (FlowEdgeID id : graph.outgoing(current)) {
      if (++examined > MaxWork) {
        search.exhausted = true;
        return search;
      }
      const auto &edge = graph.edge(id);
      if (!edge.enabled || edge.objects.empty() || !edge.objects.contains(object))
        continue;
      FlowNodeID next = edge.to;
      const auto &target = graph.node(next);
      if (!target.object || *target.object != object) continue;
      if (hasEvent(target.events, Event::Allocate) &&
          target.certainty == Certainty::Must)
        continue;
      Event sink = kind == DefectKind::DoubleFree ? Event::Release :
                                                   Event::Dereference;
      if (hasEvent(target.events, sink) && !reported[next]) {
        reported[next] = true;
        QueryResult finding;
        finding.status = QueryStatus::Found;
        finding.message = "potential defect in the supplied graph abstraction";
        finding.nodes.push_back(current);
        finding.edges.push_back(id);
        while (parent[finding.nodes.back()] != InvalidID) {
          FlowNodeID node = finding.nodes.back();
          finding.edges.push_back(incoming[node]);
          finding.nodes.push_back(parent[node]);
        }
        std::reverse(finding.nodes.begin(), finding.nodes.end());
        std::reverse(finding.edges.begin(), finding.edges.end());
        finding.nodes.push_back(next);
        finding.automatonStates.assign(finding.nodes.size(), 0);
        search.findings.push_back(std::move(finding));
        if (firstOnly) return search;
      }
      if (visited[next]) continue;
      visited[next] = true;
      parent[next] = current;
      incoming[next] = id;
      queue.push_back(next);
    }
  }
  return search;
}

QueryResult firstResult(const ResourceSearch &search, const TraceFlowGraph &graph) {
  if (!search.findings.empty()) return search.findings.front();
  QueryResult result;
  result.status = search.exhausted || !graph.complete() ? QueryStatus::Unknown :
                                                       QueryStatus::NotFound;
  result.message = search.exhausted ? "resource reachability budget exhausted" :
                   graph.complete() ? "no resource witness in the supplied graph" :
                                      "incomplete model or omitted effects";
  return result;
}

DefectReport makeReport(const TraceFlowGraph &graph, DefectKind kind,
                        QueryResult result, bool specialized) {
  DefectReport report;
  report.kind = kind;
  report.result = std::move(result);
  report.specialized = specialized;
  for (auto id : report.result.nodes) {
    NativeID native = graph.node(id).native;
    if (native != NoNativeID) report.nativeWitness.push_back(native);
  }
  return report;
}
} // namespace

namespace lotus {
namespace usetracessa {

DefectReport DefectDetector::run(DefectKind kind,
                                 std::vector<FlowNodeID> roots,
                                 std::vector<FlowNodeID> uses) const {
  DefectReport report;
  report.kind = kind;
  Query query;
  bool factsPresent = true;
  switch (kind) {
  case DefectKind::DoubleFree:
    query = queries::doubleFree(Graph);
    factsPresent = !Graph.select(Event::Release).empty();
    break;
  case DefectKind::UseAfterFree:
    query = queries::useAfterFree(Graph);
    factsPresent = !Graph.select(Event::Release).empty() &&
                   !Graph.select(Event::Dereference).empty();
    break;
  case DefectKind::Taint:
    query = queries::taint(Graph);
    factsPresent = !Graph.select(Event::Source).empty() &&
                   !Graph.select(Event::Sink).empty();
    break;
  case DefectKind::UncheckedUse:
    query = queries::uncheckedUse(std::move(roots), std::move(uses));
    factsPresent = !query.sources.empty() && !query.sinks.empty();
    break;
  }
  if (!factsPresent) {
    report.result.status = QueryStatus::Unknown;
    report.result.message = "required defect facts were not supplied";
    return report;
  }
  if ((kind == DefectKind::DoubleFree || kind == DefectKind::UseAfterFree) &&
      canUseResourceReachability(Graph)) {
    report.result = firstResult(searchResources(Graph, kind, true), Graph);
    report.specialized = true;
  } else {
    report.result = QueryEngine(Graph).run(query);
  }
  for (auto id : report.result.nodes) {
    NativeID native = Graph.node(id).native;
    if (native != NoNativeID) report.nativeWitness.push_back(native);
  }
  return report;
}

DefectScan DefectDetector::scan(DefectKind kind) const {
  if (kind != DefectKind::DoubleFree && kind != DefectKind::UseAfterFree)
    throw std::invalid_argument("UseTraceSSA: scan supports resource rules only");
  DefectScan scan;
  auto releases = Graph.select(Event::Release);
  auto sinks = Graph.select(kind == DefectKind::DoubleFree ? Event::Release :
                                                            Event::Dereference);
  if (releases.empty() || sinks.empty()) {
    scan.status = QueryStatus::Unknown;
    scan.message = "required defect facts were not supplied";
    return scan;
  }
  if (canUseResourceReachability(Graph)) {
    ResourceSearch search = searchResources(Graph, kind, false);
    scan.specialized = true;
    scan.exhaustive = !search.exhausted;
    for (auto &finding : search.findings)
      scan.findings.push_back(makeReport(Graph, kind, std::move(finding), true));
  } else {
    Query query = kind == DefectKind::DoubleFree ? queries::doubleFree(Graph) :
                                                    queries::useAfterFree(Graph);
    for (FlowNodeID sink : sinks) {
      query.sinks = {sink};
      QueryResult result = QueryEngine(Graph).run(query);
      if (result.status == QueryStatus::Found)
        scan.findings.push_back(makeReport(Graph, kind, std::move(result), false));
      else if (result.status == QueryStatus::Unknown)
        scan.exhaustive = false;
    }
  }
  scan.status = !scan.findings.empty() ? QueryStatus::Found :
                scan.exhaustive && Graph.complete() ? QueryStatus::NotFound :
                                                      QueryStatus::Unknown;
  scan.message = scan.exhaustive ? "modeled sites visited" :
                                   "search budget or model prevented a full scan";
  return scan;
}

} // namespace usetracessa
} // namespace lotus
