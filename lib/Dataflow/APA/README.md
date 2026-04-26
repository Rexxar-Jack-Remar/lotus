# Algebraic Program Analysis (the Elimination Method for Dataflow Analysis)

This directory provides an **elimination-based** intraprocedural solver that computes dataflow solutions by progressively summarizing paths, conceptually similar to state elimination in automata / Gaussian elimination over path expressions.

## Positioning: what "APA" means here

The directory is named `APA` because it implements an **algebraic, path-expression-based**
style of analysis, including paper-style ADT elimination variants on reducible flowgraphs.

However, this is **not** a full general-purpose implementation of all "Algebraic Program
Analysis" machinery. In Lotus today, this component should be read as:

- an **intraprocedural** APA-inspired elimination engine,
- specialized to LLVM function CFGs,
- aimed at MOP-style dataflow clients (reachable, const-prop, RD, liveness, etc.).

For broader interprocedural formulations, see other frameworks in this repository
(e.g., IFDS/IDE, WPDS, and NPA modules).

## Public header layout

The public API lives under `include/Dataflow/APA/`:

```text
include/Dataflow/APA/
├── APA.h                          # Canonical umbrella for the framework
├── Core/                          # Generic problem, path-expression, options, results
├── Solver/                       # Solver facade and concrete elimination engines
├── Adapters/LLVM/                 # LLVM CFG adapters
├── Analyses/LLVM/Intra/            # Concrete LLVM analyses
└── Passes/                        # Legacy-pass wrappers
```

The current layout is the supported public header structure; there are no
compatibility aliases for an older pre-reorg layout.

### Quick include guide

- Framework umbrella: `#include "Dataflow/APA/APA.h"`
- Minimal framework surface: `Core/Problem.h`, `Core/Result.h`,
  `Solver/Solver.h`, `Adapters/LLVM/ForwardProblem.h`
- LLVM clients: `Analyses/LLVM/Intra/*.h`
- Passes: `#include "Dataflow/APA/Passes/EliminationPasses.h"`
- Internal engine headers: `Solver/SolverContext.h` and the concrete
  `*Solver.h` files are solver internals; downstream clients should normally
  include only `Solver/Solver.h`.

## References

### Classical Elimination-Based Dataflow Analysis

- Static Analysis by Elimination. Pavle Subotic, Andrew E. Santosa,  and
Bernhard Scholz.
- ETAPS ’07:  A new elimination-based data flow analysis framework using
annotated decomposition trees. B. Scholz and J. Blieberger.
- CSUR 86: Elimination Algorithms for Data Flow Analysis. Babara Ryder and Marvin Paull.

### Algebraic Program Analysis (Reps & Kincaid)

- CAV 2021: Algebraic Program Analysis (Tutorial)
- POPL 19: Refinement of Path Expressions for Static Analysis. John Cypher, Jason Breck, Zak Kincaid, Thomas Reps.
- PLDI 2017: Compositional Recurrence Analysis Revisited
- FMCAD 2015: Compositional Recurrence Analysis 


## What it computes

The solver constructs **path expressions** (regular-expression-like ASTs) over edge transfer functions (`Atom`, `Union`, `Concat`, `Star`) and then evaluates those expressions over your lattice using your `meet` and `applyTransfer`.

This corresponds to a **meet-over-all-paths (MOP)** computation. For classic distributive frameworks, MOP equals the standard maximal fixed point (MFP) solution.

## Relation to `Support/Algorithms/PathExpressions`

Lotus also ships a generic path-expression utility under
`include/Support/Algorithms/PathExpressions/`. That component computes ordinary
regular expressions over edge labels in arbitrary labeled graphs.

The APA solver is different:

- APA path expressions carry **transfer functions**, not plain labels.
- APA expressions are **evaluated over a dataflow lattice** via
  `applyTransfer`, `meet`, and `maxStarIterations`.
- The utility path-expression library is for **regex/path summarization** and is
  not a drop-in implementation of the APA solver.

## Layering

- `Core/` is generic and does not depend on LLVM.
- `Solver/` is generic and builds/evaluates path expressions.
- `Adapters/LLVM/` maps LLVM CFGs into the generic problem interface.
- `Analyses/LLVM/` supplies lattice semantics and transfer behavior for concrete
  analyses.

APA is therefore a generic elimination framework, not a complete
"analysis generator." Each client analysis still provides its own lattice and
LLVM-specific modeling.

## Current gaps / non-goals

- **Not interprocedural**: this solver works within a single function CFG.
- **Not a universal semiring-equation engine**: it uses path-expression elimination with
  problem-defined `meet`/`applyTransfer`, rather than exposing the full range of algebraic
  solver variants used across APA literature.
- **ADT methods are conditional**: `ADTSimple` / `ADTDelayed` require reducible-graph
  assumptions; the solver falls back to `StateElimination` when assumptions do not hold.
- **No claim of complete APA feature parity**: this module does not attempt to cover all
  formulations (e.g., every interprocedural or Newtonian/tensor-product variant).
- **Engineering tradeoff**: path-expression growth can still be substantial on large CFGs;
  this module focuses on practical intraprocedural analyses rather than full APA scalability
  research coverage.

## Solver methods

Three elimination-style solvers are exposed via `elimination::EliminationOptions`:

- `StateElimination` (default): generic **O(n³)** state-elimination over all nodes (Floyd–Warshall-style).
- `ADTSimple`: **paper-style ADT "simple" algorithm** for **reducible** flowgraphs (O(n²) updates).
- `ADTDelayed`: **paper-style ADT "delayed" algorithm** for **reducible** flowgraphs.

`EliminationOptions` also controls non-convergent `Star` behavior:

- `NonConvergentStarPolicy = Fail | ReturnLast | ReturnIdentity`
- `MaxStarIterations` (0 means use `Problem.maxStarIterations()`).

The public facade `Solver/Solver.h` dispatches to one of three engine headers
in `include/Dataflow/APA/Solver/`:

- `SolverContext.h` (shared internals: reducible-view construction, ADT
  building, expression evaluation)
- `StateEliminationSolver.h`
- `ADTSimpleSolver.h`
- `ADTDelayedSolver.h`

Roughly, the split is:

- `Solver.h`: API surface and fallback policy
- `StateEliminationSolver.h`: generic full-CFG elimination
- `ADTSimpleSolver.h`: eager leaf-update ADT evaluation
- `ADTDelayedSolver.h`: deferred prefix composition with union-find style links

For ADT-based methods, you can optionally implement
`elimination::IntraReducibleEliminationProblem`
(dominators + topological order + edge list). If not provided, the solver computes reducible
flowgraph metadata internally and falls back to `StateElimination` when reducibility assumptions fail.

The synthesized reducible view accepts ADT only when all nodes are entry-reachable,
immediate dominators are computable, and the non-back-edge subgraph is acyclic
with entry first in topological order.


## Intraprocedural LLVM analyses

We provide a few concrete LLVM IR analyses implemented on top of the
elimination framework. These are intended as practical clients (as in the
paper), and serve as examples for adding additional analyses:

- Reachability (`runIntraElimReachable`)
- Constant propagation (`runIntraElimConstantPropagation`)
- Uninitialized variables (`runIntraElimUninitVariables`)
- Reaching definitions (`runIntraElimReachingDefinitions`)
- Available expressions (`runIntraElimAvailableExpressions`)
- Live variables (`runIntraElimLiveVariables`)
- Lockset analysis (`runIntraElimLockset`)
- Very busy expressions (`runIntraElimVeryBusyExpressions`)
- Non-null propagation (`runIntraElimNonNull`)
- Sign analysis (`runIntraElimSignAnalysis`)

Interprocedural call-string clients are also available for selected analyses,
including may-lockset analysis (`runInterElimLockset`).

## LLVM pass wrappers

For convenient use under LLVM's legacy pass manager, ten function passes are
provided:

- `-elim-reachable` (reachability)
- `-elim-constprop` (constant propagation)
- `-elim-rd` (reaching definitions)
- `-elim-available` (available expressions)
- `-elim-uninit` (uninitialized variables)
- `-elim-live` (live variables)
- `-elim-lockset` (may-lockset analysis)
- `-elim-busy` (very busy expressions)
- `-elim-nonnull` (nonnull propagation)
- `-elim-sign` (sign analysis)

Use `-elim-method=state|adt-simple|adt-delayed` to select the solver.
Printing is optional via:

- `-elim-reachable-print`
- `-elim-constprop-print`
- `-elim-rd-print`
- `-elim-available-print`
- `-elim-uninit-print`
- `-elim-live-print`
- `-elim-lockset-print`
- `-elim-busy-print`
- `-elim-nonnull-print`
- `-elim-sign-print`

Memory modeling can be toggled with:

- `-elim-use-memssa` (default: true) — use MemorySSA to refine memory analyses

When print flags are enabled, pass output now includes solver diagnostics:
status, requested/executed method, ADT fallback reason, and star-iteration
counters.

## Solver status and result lookup

- `IntraEliminationSolver::solve()` returns `SolveStatus`:
  `Ok`, `FallbackToState`, `NonConvergentStar`, `InvalidProblem`.
- `IntraEliminationSolver::getDiagnostics()` reports method/fallback/counters.
- `DataFlowResultT` uses explicit read lookup:
  - `containsNode(node)`
  - `tryIN(node)` (nullable pointer)
  and no longer returns implicit default facts for missing nodes.

## Analysis coverage notes

Constant propagation now tracks full LLVM `Constant*` values (integers, floats,
vectors, aggregates), uses LLVM constant-folding and instruction-simplification
when operands are constant, and performs alias-aware memory updates when
`AAResults` are available. Uninitialized-variable tracking normalizes pointer
bases, uses ValueTracking for guaranteed-non-undef checks, and clears aliasing
locations via `AAResults` when available, plus basic mem intrinsics.
