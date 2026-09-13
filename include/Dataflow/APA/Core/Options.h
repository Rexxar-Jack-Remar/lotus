#ifndef DATAFLOW_ELIMINATION_CORE_OPTIONS_H_
#define DATAFLOW_ELIMINATION_CORE_OPTIONS_H_

#include <cstddef>

#include "Dataflow/APA/EAN/Budget.h"
#include "Dataflow/APA/EAN/CostModel.h"
#include "Dataflow/APA/EAN/ExtractOptions.h"
#include "Dataflow/APA/EAN/LawProfile.h"

namespace elimination {

enum class EliminationMethod {
  // Generic O(n^3) state-elimination (Floyd–Warshall-style) over all nodes.
  StateElimination,
  // Paper-style ADT "simple" algorithm path-expression updates.
  ADTSimple,
  // Paper-style ADT + path-expression construction (requires reducible info).
  ADTDelayed,
};

// Pivot-order policy for the state-elimination engine (paper's "Order"
// configuration). The order never changes the final all-pairs result (the
// k-loop is a Floyd–Warshall closure), only the peak construction cost — see
// Solver/EliminationOrder.h.
enum class OrderingPolicy {
  // Baseline: reverse-topological order for reducible problems, identity
  // otherwise (unchanged historical behavior).
  Default,
  // Cost-aware greedy minimum-product ordering that minimizes the
  // predecessor–successor product driving Eq. 1's intermediate growth.
  CostAware,
};

enum class OnNonConvergentStar {
  // Abort solve with NonConvergentStar status.
  Fail,
  // Return the last iterand at the iteration bound.
  ReturnLast,
  // Return domain bottom at the iteration bound.
  ReturnIdentity,
};

enum class SolveStatus {
  Ok,
  FallbackToState,
  NonConvergentStar,
  InvalidProblem,
};

enum class FallbackReason {
  None,
  ADTRejected,
  InvalidProblem,
};

// The first structural precondition that rejected an ADT solve.  Keeping this
// separate from FallbackReason preserves the coarse API while making fallback
// hotspots actionable when profiling real CFGs.
enum class ADTRejectionReason {
  None,
  EmptyTopologicalOrder,
  DisconnectedFromEntry,
  NonBackEdgeCycle,
  EntryNotFirst,
  MissingTopologicalNode,
  InvalidImmediateDominator,
  ADTConstructionFailed,
  MissingADTLeaf,
  EdgeClassificationFailed,
  ForwardEdgeMissesIntervalEntry,
  BackEdgeMissesIntervalEntry,
};

struct SolveDiagnostics final {
  bool used_adt = false;
  EliminationMethod requested_method = EliminationMethod::StateElimination;
  EliminationMethod executed_method = EliminationMethod::StateElimination;
  FallbackReason fallback_reason = FallbackReason::None;
  ADTRejectionReason adt_rejection_reason = ADTRejectionReason::None;
  std::size_t star_iterations_total = 0;
  bool max_star_hit = false;
  // A client removed requested EAN laws that its transfer semantics do not satisfy.
  bool ean_laws_restricted = false;
  // Peak unique path-expression DAG nodes reachable from the WHOLE elimination
  // matrix at any point during construction (state elimination only). Populated
  // only when EliminationOptions::MeasurePeakNodes is set; 0 otherwise. This is
  // the paper's RQ3 "peak construction nodes" metric.
  std::size_t peak_matrix_nodes = 0;
  // Stage timings (microseconds), recorded by the state-elimination path:
  //   generation    = build matrix + eliminate intermediates (raw DAG),
  //   normalization = EAN optimization pass (0 unless EnableEAN),
  //   interpretation = evaluating summaries into client facts.
  // These fill the paper's Table VII stage columns.
  std::size_t gen_time_us = 0;
  std::size_t norm_time_us = 0;
  std::size_t interp_time_us = 0;
};

struct EliminationOptions final {
  EliminationMethod Method = EliminationMethod::StateElimination;
  // Pivot-order policy for the state-elimination engine. Default preserves the
  // historical baseline order; CostAware selects the paper's "Order" policy.
  OrderingPolicy Ordering = OrderingPolicy::Default;
  OnNonConvergentStar NonConvergentStarPolicy = OnNonConvergentStar::Fail;
  // 0 means "use Problem.maxStarIterations()".
  std::size_t MaxStarIterations = 0;
  // Reserved for future conditional collection. Diagnostics are currently
  // recorded unconditionally by the solver and attached to result metadata.
  bool RecordDiagnostics = true;

  // Opt-in RQ3 instrumentation: when set, the state-elimination engine records
  // SolveDiagnostics::peak_matrix_nodes (peak unique DAG nodes across the whole
  // matrix during construction). Default off — adds a per-pivot reachability
  // scan, so it is enabled only by the evaluation harness.
  bool MeasurePeakNodes = false;

  // EAN (Equality-saturation Algebraic Normalizer) post-optimization. When
  // enabled, the solver runs EAN on the batch of path-expression summaries
  // before interpreting them (see SolverContext::applyEAN). Default off, so the
  // baseline "Default" configuration is unchanged. The default law profile is
  // universally safe (left distributivity only); distributive clients may set a
  // richer profile (RightDistributive/Sliding/...) per their algebra.
  bool EnableEAN = false;
  ean::LawProfile EANLaws = ean::LawProfile::safeMinimal();
  ean::CostModel EANCost = ean::CostModel::uniform();
  ean::Budget EANBudget = ean::Budget::unbounded();
  ean::ExtractOptions EANExtract = {};
  // Greedy post-pass (paper's "Greedy" config): deterministic one-pass prefix
  // factorization, no e-graph, no retained alternatives. Mutually exclusive
  // with EnableEAN (EAN takes precedence if both set). Default off.
  bool EnableGreedy = false;
  // Invocation gate: run EAN only on batches whose raw unique-node count is at
  // least this (0 = always run). Copied into EANExtract.gateMinNodes at use.
  std::size_t EANMinNodes = 0;
  // Monotone guard: EAN returns the input if its output has more unique nodes
  // (never degrade an already-compact input). Copied into EANExtract at use.
  bool EANMonotone = false;
  // Interpret each summary this many times (>=1). Amortization knob for RQ2:
  // reveals the per-query interpretation cost so EAN's cheaper IR can be
  // weighed against its one-time normalization cost. Does not change results.
  std::size_t InterpRepeat = 1;

  // Memoizing (transformer-composition) interpretation. When set, the generic
  // engines SKIP the tree-walking interpreter — they still build/optimize the
  // path-expression ExprTo batch, but leave IN facts for the CLIENT to fill via
  // a memoizing interpreter that evaluates each unique DAG node once (cost ∝
  // unique nodes instead of the expanded tree). This only makes sense for a
  // relational client whose fact is a transformer and whose initialFact is the
  // compositional unit (e.g. the affine-equalities client); other clients must
  // not set it (their IN facts would be left empty). Default off.
  bool InterpMemo = false;
};

// EAN/Greedy post-optimization configuration for the interprocedural
// path-summary solver (ForwardInterSummarySolver). Mirrors the EAN-relevant
// subset of EliminationOptions so the intra and inter stories share identical
// knobs and defaults. All fields default to a no-op pass (EnableEAN and
// EnableGreedy both false), so the interprocedural baseline is unchanged.
//
// Soundness note: the interprocedural summary interpreter composes atoms the
// same way as the intra interpreter (Concat = sequential apply, Union = merge),
// so left distributivity holds unconditionally and the default safe-minimal law
// profile preserves every client's results. Distributive clients may opt into a
// richer profile per their algebra.
struct InterEANOptions final {
  // Run EAN on the batch of context summaries before interpreting them.
  bool EnableEAN = false;
  ean::LawProfile EANLaws = ean::LawProfile::safeMinimal();
  ean::CostModel EANCost = ean::CostModel::uniform();
  ean::Budget EANBudget = ean::Budget::unbounded();
  ean::ExtractOptions EANExtract = {};
  // Greedy post-pass (paper's "Greedy" config). Mutually exclusive with
  // EnableEAN (EAN takes precedence if both set). Default off.
  bool EnableGreedy = false;
  // Invocation gate: run EAN only when the batch's raw unique-node count is at
  // least this (0 = always). Copied into EANExtract.gateMinNodes at use.
  std::size_t EANMinNodes = 0;
  // Monotone guard: return the input batch if EAN's output has more unique
  // nodes. Copied into EANExtract.monotoneGuard at use.
  bool EANMonotone = false;
  // Interpret each summary this many times (>=1). Amortization knob for RQ2.
  std::size_t InterpRepeat = 1;
};

} // namespace elimination

#endif // DATAFLOW_ELIMINATION_CORE_OPTIONS_H_
