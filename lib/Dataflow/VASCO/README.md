# VASCO for Lotus

This directory contains Lotus's C++ port of **VASCO**, the value-context
interprocedural data-flow framework from:

- Rohan Padhye and Uday P. Khedker, *Interprocedural Data Flow Analysis in
  Soot using Value Contexts* (SOAP 2013).

The port preserves the paper's central idea: analyze a procedure in a
**value context** instead of a call-string context. A value context is the pair
`<method, entry-flow-value>` for forward analyses and, dually,
`<method, exit-flow-value>` for backward analyses. When two calls reach the same
procedure with the same context value, they reuse the same context and the same
computed summary. This gives precise valid-path propagation, including recursive
programs, without requiring distributive flow functions.

## Why this exists

VASCO fills the gap between:

- context-insensitive solvers, which merge unrelated callers too early, and
- IFDS/IDE-style frameworks, which require distributive transfer functions.

The framework instead assumes:

- a finite lattice of data-flow values, and
- monotone transfer functions.

That makes it suitable for analyses such as sign analysis, copy-constant
propagation, and points-to analysis, including clients whose transfer functions
operate on the whole abstract state rather than on independent facts.

## Mapping from the paper to the code

The generic implementation lives under `include/Dataflow/VASCO/` and is mostly
header-only so it stays parameterized over method, CFG-node, and lattice types.

- `Core/ProgramRepresentation.h`
  abstracts entry points, control-flow graphs, and call-target resolution.
- `Core/Context.h`
  stores one value context, including entry/exit values, per-node IN/OUT maps,
  and a pseudo-topologically ordered worklist.
- `Core/ContextTransitionTable.h`
  records caller-context/call-site to callee-context transitions, matching the
  transition relation described in the paper.
- `Core/InterProceduralAnalysis.h`
  owns the global set of contexts, the transition table, and utilities such as
  `getContexts()` and `getMeetOverValidPathsSolution()`.
- `Solver/ForwardInterProceduralAnalysis.h`
  implements the forward algorithm corresponding to Figure 1 in the paper.
- `Solver/BackwardInterProceduralAnalysis.h`
  provides the backward dual of the same algorithm.
- `Solver/OldForwardInterProceduralAnalysis.h`
  keeps the older stack-driven forward engine used by the migrated LLVM
  points-to client.

In the forward solver, call handling is split exactly along the paper's API:

- `callEntryFlowFunction(...)` computes the callee entry value,
- `callExitFlowFunction(...)` maps the callee summary back to the caller,
- `callLocalFlowFunction(...)` preserves local effects across the call,
- `normalFlowFunction(...)` handles non-call instructions.

When a callee context finishes and its entry/exit summary changes,
`wakeCallers(...)` re-enqueues the corresponding call sites so the updated
summary is propagated to all valid callers.

The opt-in parallel scheduler keeps the same value-context semantics but treats
contexts as schedulable tasks. It uses per-callsite summary-version observations
to replay only stale caller call sites when a callee summary changes. See
`ParallelContextScheduler.md` for the algorithm and invariants.

## Result forms

The framework exposes two useful result views:

- **context-sensitive** results via `Context::getValueBefore()` and
  `Context::getValueAfter()` for a particular value context, and
- **meet-over-valid-paths** results via
  `InterProceduralAnalysis::getMeetOverValidPathsSolution()`, which merges all
  surviving contexts for each CFG node.

This mirrors the distinction in the paper between explicit value contexts and
the final merged solution over valid interprocedural paths.

## LLVM instantiation in Lotus

Lotus provides an LLVM-facing instantiation of the generic framework:

- `Adapters/LLVM/DefaultLLVMProgramRepresentation.h`
  adapts LLVM IR to VASCO using instruction-level CFGs and direct-call
  resolution, with an optional custom resolver hook for stronger call-graph
  information.
- `Analyses/LLVMSignAnalysis.h`
  ports the paper's sign-analysis example to LLVM IR.
- `Analyses/LLVMCopyConstantAnalysis.h`
  ports copy-constant propagation.
- `Analyses/LLVMLiveVariablesAnalysis.h`
  adds a backward interprocedural liveness client over LLVM SSA values.
- `Analyses/LLVMNullnessAnalysis.h`
  tracks whether pointer-valued SSA results are null, non-null, or maybe-null.
- `Analyses/LLVMPointsToAnalysis.h`
  ports the original points-to/call-graph client to LLVM using allocation-site
  objects, field-sensitive byte offsets, and function-pointer target
  resolution.

## LLVM-specific behavior and limits

- The generic value-context framework from the paper is present in Lotus,
  including context reuse, recursion handling, forward/backward propagation, and
  caller reprocessing when callee summaries improve.
- The default LLVM program representation is intentionally lightweight. Out of
  the box it resolves direct calls and treats unresolved or declaration-only
  callees conservatively.
- Unknown or indirect calls without a stronger resolver fall back to
  conservative call-local handling.
- The LLVM points-to client is field-sensitive for constant-offset accesses, but
  it remains conservative for unknown offsets, external-library behavior, and
  richer heap models.
- For production use, clients that need more precise indirect-call handling are
  expected to plug Lotus alias-analysis or call-graph information into the
  `DefaultLLVMProgramRepresentation` resolver callback.

## Practical extension points

To add a new VASCO-based analysis in Lotus:

1. Choose `ForwardInterProceduralAnalysis` or
   `BackwardInterProceduralAnalysis`.
2. Define the lattice operations: `topValue()`, `boundaryValue()`, `copy()`,
   and `meet()`.
3. Implement the direction-specific flow hooks.
4. Supply a `ProgramRepresentation` for your IR and call-resolution policy.
5. Run `doAnalysis()` and inspect either per-context or merged results.

For user-facing Sphinx documentation, see `docs/source/dataflow/vasco.rst`.
