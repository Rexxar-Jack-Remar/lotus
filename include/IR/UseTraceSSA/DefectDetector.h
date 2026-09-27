#ifndef LOTUS_IR_USETRACESSA_DEFECTDETECTOR_H
#define LOTUS_IR_USETRACESSA_DEFECTDETECTOR_H

#include "IR/UseTraceSSA/Models.h"

namespace lotus {
namespace usetracessa {

enum class DefectKind {
  DoubleFree,
  UseAfterFree,
  Taint,
  Heartbleed,
  UncheckedUse
};

struct DefectReport {
  DefectKind kind = DefectKind::DoubleFree;
  QueryResult result;
  bool specialized = false;
  /// Native SVFG IDs present on witness nodes. History psi/phi nodes have no
  /// native ID, so this list need not have one entry per witness node.
  std::vector<NativeID> nativeWitness;
};

struct DefectScan {
  QueryStatus status = QueryStatus::NotFound;
  std::vector<DefectReport> findings;
  bool specialized = false;
  bool exhaustive = true;
  std::string message;
};

/// Run a defect rule over an already constructed and annotated UseTraceSSA graph.
/// Found means a potential defect in the supplied abstraction. No required
/// facts, incomplete modeling, or exhausted budgets yield Unknown, never Safe.
/// Clients can annotate events or append object histories before calling run.
class DefectDetector {
public:
  explicit DefectDetector(const TraceFlowGraph &graph) : Graph(graph) {}
  DefectReport run(DefectKind kind,
                   std::vector<FlowNodeID> roots = {},
                   std::vector<FlowNodeID> uses = {}) const;
  /// Enumerate one witness per release/dereference site for a resource rule.
  /// An issue can make the negative result Unknown even after all modeled
  /// sites have been visited.
  DefectScan scan(DefectKind kind) const;

private:
  const TraceFlowGraph &Graph;
};

} // namespace usetracessa
} // namespace lotus
#endif
