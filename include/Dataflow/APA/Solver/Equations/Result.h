#pragma once

#include "Dataflow/APA/Core/PathExpr.h"
#include "Dataflow/APA/Solver/Equations/Options.h"

#include <map>

namespace elimination {

template <typename KeyT, typename TransferT>
class PathSummaryEquationResult final {
public:
  using expr_ref_t = typename PathExprFactory<TransferT>::Ref;

  const expr_ref_t *lookup(const KeyT &Key) const {
    auto It = Summaries.find(Key);
    return It == Summaries.end() ? nullptr : &It->second;
  }

  const std::map<KeyT, expr_ref_t> &summaries() const { return Summaries; }
  // Non-const access so a post-solve optimization pass (EAN/Greedy in
  // ForwardInterSummarySolver) can replace each context's summary expression
  // in place with a semantically-equivalent, cost-minimized form before
  // interpretation. Keys are never added or removed through this handle.
  std::map<KeyT, expr_ref_t> &summaries() { return Summaries; }
  const PathSummaryEquationDiagnostics &diagnostics() const {
    return Diagnostics;
  }

private:
  template <typename K, typename T> friend class PathSummaryEquationSolver;

  std::map<KeyT, expr_ref_t> Summaries;
  PathSummaryEquationDiagnostics Diagnostics;
};

} // namespace elimination
