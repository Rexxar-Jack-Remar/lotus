#pragma once

// PathCostFn: an egg-compatible cost function turning CostModel's per-operator
// weights into a per-e-class best node/cost (the "profiled tree cost"). Split
// from CostModel.h so that the plain CostModel struct can live in Core's
// EliminationOptions without dragging egg's Extract.h into Core.

#include "Dataflow/APA/EAN/CostModel.h"
#include "Dataflow/APA/EAN/PathLang.h"
#include "Solvers/EGraph/Extract.h"

namespace elimination {
namespace ean {

struct PathCostFn
    : ::lotus::egraph::CostFunction<PathCostFn, PathLang, double> {
  using Cost = double;

  CostModel model;

  PathCostFn() = default;
  explicit PathCostFn(CostModel m) : model(m) {}

  double opWeight(const PathLang &n) const {
    if (isZero(n)) return model.wZero;
    if (isOne(n)) return model.wOne;
    if (isAtom(n)) return model.wAtom;
    if (isStar(n)) return model.wStar;
    if (isJoin(n)) return model.wJoin;
    return model.wSeq; // seq
  }

  template <typename C> double cost(const PathLang &node, C &&child_cost) {
    double total = opWeight(node);
    for (Id child : node.children()) {
      total += child_cost(child);
    }
    return total;
  }
};

} // namespace ean
} // namespace elimination

