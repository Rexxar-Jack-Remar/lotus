#ifndef LOTUS_IR_UFG_DEFECTDETECTOR_H
#define LOTUS_IR_UFG_DEFECTDETECTOR_H

#include "IR/UFG/UFG.h"
#include "IR/UseTraceSSA/DefectDetector.h"

namespace lotus {
namespace ufg {

using usetracessa::DefectKind;
using usetracessa::DefectReport;
using usetracessa::DefectScan;

/// Resource scans run one UFG tabulation per object and collect every accepted
/// site. Automata and report types remain compatible with UseTraceSSA.
class DefectDetector {
public:
  explicit DefectDetector(const UFGGraph &graph,
                          std::optional<std::size_t> contextLimit = std::nullopt)
      : Graph(graph), ContextLimit(contextLimit) {}
  DefectReport run(DefectKind kind, std::vector<FlowNodeID> roots = {},
                   std::vector<FlowNodeID> uses = {}) const;
  DefectScan scan(DefectKind kind) const;

private:
  const UFGGraph &Graph;
  std::optional<std::size_t> ContextLimit;
};

} // namespace ufg
} // namespace lotus
#endif
