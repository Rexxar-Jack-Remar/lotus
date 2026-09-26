#pragma once

// Knobs for reuse-aware batch extraction (paper Algorithm 2) and for how the
// saturation driver measures its plateau signal.

#include <cstddef>

namespace elimination {
namespace ean {

struct ExtractOptions {
  std::size_t reuseIters = 3;   // K: reuse-refinement iterations
  double discountLambda = 0.5;  // λ: sublinear sharing discount rate
  std::size_t relaxPasses = 0;  // cycle-safe relaxation passes; 0 = auto (#classes+1)

  // Which cost the saturation loop uses to detect a plateau. Tree is cheap
  // (M3 egg tree cost); Dag runs the full reuse-aware Eq. 5 objective each
  // round (faithful to Algorithm 1, but K·I× more expensive). Switchable so the
  // choice can be made from evaluation data (RQ4).
  enum class PlateauCost { Tree, Dag };
  PlateauCost plateauMode = PlateauCost::Tree;

  // Invocation gate (paper §IV-C): skip EAN entirely when the raw batch has
  // fewer than this many unique DAG nodes (0 = no gate). Below the threshold
  // saturation cannot repay its overhead, so ean() returns the input verbatim.
  std::size_t gateMinNodes = 0;

  // Monotone guard: if the exported batch has MORE unique nodes than the input,
  // return the input verbatim. Extends I3 (root preservation) to output-quality
  // preservation, so EAN never degrades an already-compact input (e.g. one
  // produced by cost-aware elimination ordering). Off by default.
  bool monotoneGuard = false;

  // Explore phase — guarded expansion (paper §III.C, Table III). Distributes a
  // JOIN sitting inside a SEQ (the reverse of factorization) to expose sharing:
  //   P·(r1 ⊕ … ⊕ rk)·S  ->  (P·r1·S) ⊕ … ⊕ (P·rk·S)
  // Admitted only when it aligns with structure already in the e-graph — at
  // least `expandMinAligned` produced branches must ALREADY exist as e-classes
  // (0 disables the alignment requirement) — and it adds at most
  // `expandGrowthCap` new e-nodes to the class. This opportunity guard keeps
  // expansion from blindly undoing factorization or exploding the e-graph.
  std::size_t expandMinAligned = 1;
  std::size_t expandGrowthCap = 64;

  // Phase scheduling. true (default) runs the phases Cleanup→Factor→Star→Explore
  // in order, each to local saturation before advancing (paper Algorithm 1).
  // false applies every rewrite family together each round (the "No phase
  // schedule" ablation for Table VIII / RQ4).
  bool scheduled = true;
};

} // namespace ean
} // namespace elimination

