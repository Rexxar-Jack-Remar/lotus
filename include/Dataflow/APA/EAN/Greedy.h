#pragma once

// Greedy: the paper's "Greedy" configuration — Default plus deterministic local
// simplification. It shares EAN's canonicalization (ACI/associativity
// normalization via the e-graph import) and its deterministic prefix
// factorization (left-distributivity, universally sound), but performs a plain
// per-class cheapest-tree extraction (reuseIters = 0): NO reuse-aware,
// cross-root cost optimization over retained competing forms. The Greedy→EAN
// gap therefore isolates exactly the value of retaining alternatives for
// cost-driven batch extraction.
//
// Implemented as the EAN pipeline restricted to {safe-minimal laws, uniform
// cost, no reuse iterations, monotone guard}. Semantics-preserving: only
// left-distributive factorization + associativity re-normalization are used, so
// every client's results are preserved (no distributivity assumption).

#include <vector>

#include "Dataflow/APA/Core/PathExpr.h"
#include "Dataflow/APA/EAN/EAN.h"

namespace elimination {

// Greedily simplify a batch of path-expression roots in factory `F`, returning
// one simplified Ref per input root (order preserved). See header comment.
template <typename TransferT>
std::vector<typename PathExprFactory<TransferT>::Ref>
greedySimplify(const std::vector<typename PathExprFactory<TransferT>::Ref> &R,
               PathExprFactory<TransferT> &F) {
  ean::ExtractOptions opts;
  opts.reuseIters = 0;       // no reuse-aware cross-root optimization
  opts.monotoneGuard = true; // never return a batch larger than the input
  return ean::ean<TransferT>(R, ean::LawProfile::safeMinimal(),
                             ean::CostModel::uniform(), ean::Budget::unbounded(),
                             F, nullptr, opts);
}

} // namespace elimination

