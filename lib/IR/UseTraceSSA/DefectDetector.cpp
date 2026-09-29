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
    auto scanResult = scanImpl(kind, true);
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
  return scanImpl(kind, false);
}

DefectScan DefectDetector::scanImpl(DefectKind kind, bool firstOnly) const {
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
  // Reuse symbolic path provenance when available. Unbounded Dyck queries
  // share witness tabulation across sinks choosing the same object.
  auto batch = QueryEngine(Graph).runObjects({q, candidates(Graph), true});
  scan.statistics = batch.statistics;
  scan.exhaustive = batch.complete;
  const Event target = kind == DefectKind::DoubleFree ? Event::Release :
                       kind == DefectKind::UseAfterFree ? Event::Dereference : Event::Exit;
  std::map<ObjectID, std::vector<FlowNodeID>> witnessSinks;
  std::map<FlowNodeID, std::vector<ObjectID>> objectsAtSink;
  for (const auto &sink : batch.foundAt) {
    // An accepting state may reach another object's sink after the event that
    // made it accepting. Report only objects with the target event here.
    std::vector<ObjectID> atSink;
    for (auto bit : sink.second.set_bits()) {
      ObjectID object = batch.universe.objects()[bit];
      if (hasEvent(Graph.effectiveEvent(sink.first, object).events, target))
        atSink.push_back(object);
    }
    if (atSink.empty()) continue;
    ObjectID object = atSink.front();
    if (firstOnly || batch.hasWitnesses()) {
      QueryResult witness;
      if (batch.hasWitnesses()) witness = batch.witness(sink.first, object);
      else {
        q.sinks = {sink.first}; q.memoryObject = object;
        witness = QueryEngine(Graph).run(q);
      }
      if (!witness.found()) {
        scan.exhaustive = false;
        continue;
      }
      auto report = makeReport(Graph, kind, std::move(witness));
      report.witnessObject = object;
      report.objects = std::move(atSink);
      scan.findings.push_back(std::move(report));
      if (firstOnly) break;
      continue;
    }
    witnessSinks[object].push_back(sink.first);
    objectsAtSink.emplace(sink.first, std::move(atSink));
  }
  std::map<FlowNodeID, DefectReport> reports;
  for (const auto &group : witnessSinks) {
    q.sinks = group.second; q.memoryObject = group.first;
    auto witnesses = QueryEngine(Graph).runToSinks(q);
    for (auto sink : group.second) {
      auto found = witnesses.foundAt.find(sink);
      if (found == witnesses.foundAt.end()) {
        scan.exhaustive = false;
        continue;
      }
      auto report = makeReport(Graph, kind, std::move(found->second));
      report.witnessObject = group.first;
      report.objects = std::move(objectsAtSink.at(sink));
      reports.emplace(sink, std::move(report));
    }
  }
  for (auto &report : reports) scan.findings.push_back(std::move(report.second));
  scan.status = !scan.findings.empty() ? QueryStatus::Found :
                scan.exhaustive ? QueryStatus::NotFound : QueryStatus::Unknown;
  scan.message = batch.message;
  return scan;
}
} // namespace usetracessa
} // namespace lotus
