#include "IR/UseTraceSSA/DefectDetector.h"
#include <stdexcept>

namespace lotus {
namespace usetracessa {
namespace {
DefectReport makeReport(const TraceFlowGraph &graph, DefectKind kind, QueryResult result) {
  DefectReport report;
  report.kind = kind; report.result = std::move(result);
  for (auto id : report.result.nodes) {
    NativeID native = graph.node(id).native;
    if (native != NoNativeID) report.nativeWitness.push_back(native);
  }
  return report;
}
ObjectUniverse candidates(const TraceFlowGraph &graph) {
  auto objects = graph.resourceCandidates();
  // Generic resource annotations have no finite guard. Keep an anonymous
  // candidate for these clients as well as for unknown resource effects.
  for (const auto &n : graph.nodes())
    if (hasEvent(n.events, Event::Allocate | Event::Release |
                               Event::Dereference | Event::Open | Event::Close)) {
      objects.push_back(UnknownResource); break;
    }
  return ObjectUniverse(std::move(objects));
}
} // namespace

DefectReport DefectDetector::run(DefectKind kind, std::vector<FlowNodeID> roots,
                                 std::vector<FlowNodeID> uses) const {
  if (kind == DefectKind::DoubleFree || kind == DefectKind::UseAfterFree ||
      kind == DefectKind::MemoryLeak || kind == DefectKind::FileLeak) {
    auto scanResult = scan(kind);
    if (!scanResult.findings.empty()) return std::move(scanResult.findings.front());
    QueryResult result; result.status = scanResult.status; result.message = scanResult.message;
    return makeReport(Graph, kind, std::move(result));
  }
  Query q;
  if (kind == DefectKind::Taint) q = queries::taint(Graph);
  else q = queries::uncheckedUse(std::move(roots), std::move(uses));
  q.contextLimit = ContextLimit;
  QueryResult result;
  if (q.sources.empty() || q.sinks.empty()) {
    result.status = QueryStatus::Unknown;
    result.message = "required defect facts were not supplied";
  } else result = QueryEngine(Graph).run(q);
  return makeReport(Graph, kind, std::move(result));
}

DefectScan DefectDetector::scan(DefectKind kind) const {
  if (kind != DefectKind::DoubleFree && kind != DefectKind::UseAfterFree &&
      kind != DefectKind::MemoryLeak && kind != DefectKind::FileLeak)
    throw std::invalid_argument("UseTraceSSA: scan supports resource rules only");
  DefectScan scan;
  Query q = kind == DefectKind::DoubleFree ? queries::doubleFree(Graph) :
            kind == DefectKind::UseAfterFree ? queries::useAfterFree(Graph) :
            kind == DefectKind::MemoryLeak ? queries::memoryLeak(Graph) :
                                             queries::fileLeak(Graph);
  q.contextLimit = ContextLimit;
  const Event required = kind == DefectKind::FileLeak ? Event::Open :
      kind == DefectKind::MemoryLeak ? Event::Allocate : Event::Release;
  if (Graph.select(required).empty() || q.sinks.empty()) {
    scan.status = QueryStatus::Unknown; scan.exhaustive = false;
    scan.message = "required defect facts were not supplied";
    return scan;
  }
  // One symbolic traversal discovers every object and sink. Only requested
  // concrete evidence uses the fixed-object oracle; no resource-specific BFS.
  auto batch = QueryEngine(Graph).runObjects({q, candidates(Graph)});
  scan.statistics = batch.statistics;
  scan.exhaustive = batch.complete;
  for (const auto &sink : batch.foundAt) {
    ObjectID object = batch.universe.objects().at(sink.second.find_first());
    q.sinks = {sink.first}; q.memoryObject = object;
    auto witness = QueryEngine(Graph).run(q);
    if (!witness.found()) {
      scan.exhaustive = false;
      continue;
    }
    auto report = makeReport(Graph, kind, std::move(witness));
    report.witnessObject = object;
    for (auto bit : sink.second.set_bits())
      report.objects.push_back(batch.universe.objects()[bit]);
    scan.findings.push_back(std::move(report));
  }
  scan.status = !scan.findings.empty() ? QueryStatus::Found :
                scan.exhaustive ? QueryStatus::NotFound : QueryStatus::Unknown;
  scan.message = batch.message;
  return scan;
}
} // namespace usetracessa
} // namespace lotus
