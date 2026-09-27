#include "IR/UseHistory/DefectDetector.h"

namespace lotus {
namespace usehistory {

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
  case DefectKind::Heartbleed:
    query = queries::heartbleed(Graph);
    factsPresent = !Graph.select(Event::Source).empty() &&
                   !Graph.select(Event::CopyLength).empty() &&
                   !Graph.select(Event::NetworkWrite).empty();
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
  report.result = QueryEngine(Graph).run(query);
  for (auto id : report.result.nodes) {
    NativeID native = Graph.node(id).native;
    if (native != NoNativeID) report.nativeWitness.push_back(native);
  }
  return report;
}

} // namespace usehistory
} // namespace lotus
