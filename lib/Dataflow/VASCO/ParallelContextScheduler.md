# Parallel VASCO Context Scheduler

## Overview

The parallel VASCO scheduler keeps the original value-context semantics and
changes only the order in which contexts are processed. A VASCO context is a
pair:

- forward analysis: `(method, entry value)`
- backward analysis: `(method, exit value)`

Sequential VASCO stores all contexts in one global worklist. The parallel
scheduler treats each context as a schedulable task. At most one worker owns a
context at a time, but different contexts can run concurrently.

The clean scheduling idea is **versioned summary replay**. Every context has a
monotonic summary version. A caller records the callee summary version it
observed at each call site. When the callee summary changes, only call sites
that observed an older version, or have not observed a version yet, are
replayed.

## State

Each context stores:

- local node worklist
- per-node IN and OUT values
- entry and exit values
- summary version
- analysed/freed flags
- a per-context mutex

The analysis owns:

- the global ready-context worklist
- the set of currently running contexts
- the context transition table
- the observed-version table

The observed-version table is keyed by:

```text
(caller context, call node, callee context) -> observed summary version
```

For forward analyses, the published summary is the callee exit value. For
backward analyses, it is the callee entry value.

## Scheduling Algorithm

The parallel scheduler runs a fixed number of worker tasks from the shared
`ThreadPool`.

```text
while work remains:
  take one ready context not owned by another worker
  lock the context
  process up to ContextStepBudget local work items
  unlock the context
  if the context still has local work:
    re-enqueue it
```

This gives context-level parallelism while avoiding concurrent mutation of a
single context's facts and worklist.

If no worker threads are configured, if parallel scheduling is disabled, or if
`FreeResultsOnTheFly` is enabled, the solver uses the sequential loop.

## Call Handling

When a call edge targets a callee context, the solver interns contexts by value:

```text
getOrCreate(method, context value)
```

This lookup and insertion is protected by the analysis mutex, so parallel
workers cannot create duplicate equivalent contexts.

If the target context has already published a summary, the caller applies that
summary and records the version it consumed:

```text
observed[(callsite, target_context)] = target_context.summary_version
```

If the target context is not analysed yet, the caller continues with its local
call flow and will be replayed when the callee publishes its first summary.

## Summary Publication

When a context reaches its synthetic final work item, it recomputes its summary:

- forward: meet over tail OUT values
- backward: meet over head IN values

If this is the first summary or the summary value changed, the context increments
its summary version and publishes it.

```text
if first_summary or summary_changed:
  summary_version++
  replay stale caller call sites
else:
  suppress caller wakeup
```

## Versioned Replay

For each caller call site of the published context:

```text
observed = observed_version[(callsite, context)]
if observed is missing or observed < context.summary_version:
  enqueue the call node in the caller context
else:
  skip replay
```

Missing observations are treated as stale because the caller may have created a
transition before the callee summary was available.

This is more precise than waking every caller on every finalization. It also
keeps the idea narrow: parallelism is expressed at the value-context level, and
replay is governed only by summary versions.

## Correctness Invariants

The scheduler relies on these invariants:

1. Only one worker mutates a context at a time.
2. Context interning is atomic with respect to `(method, context value)`.
3. Data-flow values evolve through the existing monotone `meet` logic.
4. A changed summary increments its context version.
5. Any caller that consumed an older summary is eventually replayed.
6. A call site that already consumed the current summary does not need replay.
7. The final meet-over-valid-paths solution matches the sequential solver.

The implementation preserves the existing sequential solver as the default
execution mode. Parallel scheduling is opt-in via
`setParallelContextScheduling(true)`.

## Main Implementation Points

- `include/Dataflow/VASCO/Core/Context.h`
  stores per-context locking and summary versions.
- `include/Dataflow/VASCO/Core/InterProceduralAnalysis.h`
  stores scheduler options, diagnostics, context interning, and the observed
  summary-version table.
- `include/Dataflow/VASCO/Solver/ForwardInterProceduralAnalysis.h`
  applies the scheduler to forward value contexts.
- `include/Dataflow/VASCO/Solver/BackwardInterProceduralAnalysis.h`
  applies the same scheduler to backward value contexts.
- `tests/unit/Dataflow/VASCO/VASCOParallelHarnessTest.cpp`
  checks sequential/parallel parity, equivalent-context reuse, and stale
  call-site replay under worker threads.
