#pragma once

// EAN top-level entry point: the compiler-style optimizer pass between
// path-expression construction and interpretation.
//
//   ean(R, L, C, B, F, stats, opts) : import R into a canonical e-graph, run
//   guarded/budgeted saturation, extract the cheapest equivalent shared DAG
//   under cost model C (reuse-aware, Eq. 5), and export it back into factory F
//   — one Ref per input root.
//
// Fail-safe (paper §III.D, requirement R2 / termination): if anything throws or
// the result shape is wrong, EAN returns the original root batch R unchanged.
// By root preservation (I3) this fallback is always available and never worse
// than the input for analysis semantics. An incorrect client law profile is a
// client bug and remains outside this guarantee.

#include <vector>

#include "Dataflow/APA/Core/PathExpr.h"
#include "Dataflow/APA/EAN/BatchExtract.h"
#include "Dataflow/APA/EAN/Budget.h"
#include "Dataflow/APA/EAN/CostFn.h"
#include "Dataflow/APA/EAN/CostModel.h"
#include "Dataflow/APA/EAN/DagStats.h"
#include "Dataflow/APA/EAN/Export.h"
#include "Dataflow/APA/EAN/ExtractOptions.h"
#include "Dataflow/APA/EAN/Import.h"
#include "Dataflow/APA/EAN/LawProfile.h"
#include "Dataflow/APA/EAN/Saturate.h"

namespace elimination {
namespace ean {

template <typename TransferT>
std::vector<typename PathExprFactory<TransferT>::Ref>
ean(const std::vector<typename PathExprFactory<TransferT>::Ref> &R,
    const LawProfile &L, const CostModel &C, const Budget &B,
    PathExprFactory<TransferT> &F, SaturationStats *stats = nullptr,
    ExtractOptions opts = {}) {
  // Invocation gate: below the raw-node threshold, saturation cannot repay its
  // overhead — return the input verbatim (root preservation, I3).
  if (opts.gateMinNodes > 0 &&
      computeDagStats<TransferT>(R).uniqueNodes < opts.gateMinNodes) {
    return R;
  }
  try {
    auto imp = importCanonical<TransferT>(R);

    // Plateau signal: cheap tree cost (M3) or the full reuse-aware Eq. 5 cost,
    // switchable via opts.plateauMode.
    PathCostFn tree_fn(C);
    SaturationStats st;
    if (opts.plateauMode == ExtractOptions::PlateauCost::Dag) {
      st = saturate(imp.g, imp.roots, L, B, opts,
                    [&](Graph &g, const std::vector<Id> &roots) {
                      return reuseAwareCost(g, roots, C, opts);
                    });
    } else {
      st = saturate(imp.g, imp.roots, L, B, opts,
                    [&](Graph &g, const std::vector<Id> &roots) {
                      return extractCost(g, roots, tree_fn);
                    });
    }
    if (stats) {
      *stats = st;
    }

    // Final export: reuse-aware best selection from the (grown) e-graph.
    ExtractResult sel = reuseAwareExtract(imp.g, imp.roots, C, opts);
    PickFn pick = [&sel, &imp](Id c) -> const PathLang & {
      return sel.chosen.at(imp.g.find(c).value());
    };

    auto out = exportBatch<TransferT>(imp, F, pick);
    if (out.size() != R.size()) {
      return R; // shape mismatch: fall back
    }
    // Monotone guard: never return a batch larger than the input (I3 extended
    // to output quality). Keeps EAN from degrading an already-compact input.
    if (opts.monotoneGuard &&
        computeDagStats<TransferT>(out).uniqueNodes >
            computeDagStats<TransferT>(R).uniqueNodes) {
      return R;
    }
    return out;
  } catch (...) {
    return R; // resource/internal failure: fall back to the original batch
  }
}

} // namespace ean
} // namespace elimination

