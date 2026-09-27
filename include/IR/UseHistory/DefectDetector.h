#ifndef LOTUS_IR_USEHISTORY_DEFECTDETECTOR_H
#define LOTUS_IR_USEHISTORY_DEFECTDETECTOR_H

#include "IR/UseHistory/Models.h"

namespace lotus {
namespace usehistory {

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
  /// Native SVFG IDs present on witness nodes. History psi/phi nodes have no
  /// native ID, so this list need not have one entry per witness node.
  std::vector<NativeID> nativeWitness;
};

/// Run a defect rule over an already constructed and annotated UseHistory graph.
/// Found means a potential defect in the supplied abstraction. No required
/// facts, incomplete modeling, or exhausted budgets yield Unknown, never Safe.
/// Clients can annotate events or append object histories before calling run.
class DefectDetector {
public:
  explicit DefectDetector(const FlowGraph &graph) : Graph(graph) {}
  DefectReport run(DefectKind kind,
                   std::vector<FlowNodeID> roots = {},
                   std::vector<FlowNodeID> uses = {}) const;

private:
  const FlowGraph &Graph;
};

} // namespace usehistory
} // namespace lotus
#endif
