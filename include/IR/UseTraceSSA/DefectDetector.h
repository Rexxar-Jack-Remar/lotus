#ifndef LOTUS_IR_USETRACESSA_DEFECTDETECTOR_H
#define LOTUS_IR_USETRACESSA_DEFECTDETECTOR_H

#include "IR/UseTraceSSA/Models.h"

namespace lotus {
namespace usetracessa {

enum class DefectKind {
  DoubleFree,
  UseAfterFree,
  Taint,
  UncheckedUse,
  MemoryLeak,
  FileLeak
};

struct DefectReport {
  DefectKind kind = DefectKind::DoubleFree;
  QueryResult result;
  std::optional<ObjectID> witnessObject;
  std::vector<ObjectID> objects;
  /// Native SVFG IDs present on witness nodes. History psi/phi nodes have no
  /// native ID, so this list need not have one entry per witness node.
  std::vector<NativeID> nativeWitness;
};

struct DefectScan {
  QueryStatus status = QueryStatus::NotFound;
  std::vector<DefectReport> findings;
  ObjectBatchStatistics statistics;
  bool exhaustive = true;
  std::string message;
};

/// Run a defect rule over an already constructed and annotated UseTraceSSA graph.
/// Found means a potential defect in the supplied abstraction. No required
/// facts, incomplete modeling, or exhausted budgets yield Unknown, never Safe.
/// Clients supply generic events or guarded temporal effects before calling run.
class DefectDetector {
public:
  explicit DefectDetector(const TraceFlowGraph &graph,
                          std::optional<std::size_t> contextLimit = std::nullopt)
      : Graph(graph), ContextLimit(contextLimit) {}
  DefectReport run(DefectKind kind,
                   std::vector<FlowNodeID> roots = {},
                   std::vector<FlowNodeID> uses = {}) const;
  /// Enumerate one witness per resource sink site.
  /// An issue can make the negative result Unknown even after all modeled
  /// sites have been visited.
  DefectScan scan(DefectKind kind) const;

private:
  DefectScan scanImpl(DefectKind kind, bool firstOnly) const;
  const TraceFlowGraph &Graph;
  std::optional<std::size_t> ContextLimit;
};

} // namespace usetracessa
} // namespace lotus
#endif
