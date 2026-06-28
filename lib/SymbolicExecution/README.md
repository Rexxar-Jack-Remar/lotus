# Symbolic Execution

This subtree provides Lotus's symbolic bug-checking stack. It builds a
path-sensitive analysis over the guarded value-flow graph, tracks symbolic
memory and scalar facts in `AnalysisState`, and uses SMT-backed path-condition
reasoning to decide whether a reported path is still feasible. The original
design direction came from the symbolic heap and disjointness work behind the
ISSTA 2024 BOF paper, but the implementation in Lotus has since grown into a
more general symbolic execution subsystem that supports additional bug classes.

## What It Builds

At the top level, `SymbolicExecutionWrapper` runs the analysis as a module pass
and delegates execution to `AnalysisDriver`.

The usual analysis flow is:

1. Initialize shared module context through `gvfg_utility`.
2. Build or query the per-function GVFG used as the symbolic execution graph.
3. Run `AnalysisDriver` over functions, usually in a summary-friendly order.
4. Propagate `AnalysisState` through GVFG nodes, memory operations, branches,
   and calls.
5. Represent scalar values with `PropertyValue`, `PropertyInteger`, and
   `PropertySymExpr`.
6. Use `PathCondSolver` and `ConstraintRepr` to encode branch and data
   constraints into SMT queries.
7. Apply summaries, taint specifications, and memory API models while tracking
   bug traces.

## Major Components

- `AnalysisDriver.*`: coordinates whole-module execution, collects summaries,
  and records bug traces.
- `AnalysisState.*`, `AnalysisStateQuery.*`, `AnalysisStateSummary.*`,
  `AnalysisStateTaint.*`, and `AnalysisStateUtils.*`: implement the symbolic
  abstract state, including memory objects, path-sensitive value sets, summary
  import/export, and taint-aware state updates.
- `ProgramVar.*`: defines the stable symbolic variable layer used to name GVFG
  nodes and analysis-introduced auxiliary values.
- `PropertyValue.*`, `PropertyInteger.*`, and `PropertySym.*`: define the
  scalar property domain used for offsets, sizes, comparisons, and symbolic
  arithmetic.
- `ConstraintRepr.*` and `PathCondSolver.*`: translate symbolic facts and path
  predicates into SMT expressions and satisfiability checks.
- `MemoryAPI.*` and `GVFGUtility.*`: provide allocation modeling, library
  compatibility hooks, data-layout queries, function-order helpers, and access
  to the active GVFG builder.
- `TaintModel.*`: stores source, transfer, and sink specifications used when a
  bug class depends on tainted data reaching sensitive program points.
- `SymbolicExecutionWrapper.*`: integrates the subsystem with the legacy LLVM
  pass pipeline and emits final bug reports.

## Key Concepts

- `ProgramValuePtr` and `Var` are the symbolic names that connect analysis
  state back to concrete program entities. They may wrap GVFG nodes directly or
  auxiliary values introduced by the analysis.
- `PropertySymExpr` models affine expressions over `Var`s. This is the main
  scalar language used for offsets, sizes, pointer arithmetic summaries, and
  branch comparisons.
- `AnalysisState` tracks symbolic memory through access paths, points-to items,
  guarded value sets, and path conditions rather than through raw LLVM values
  alone.
- `gvfg_utility` is the glue layer between the symbolic executor and other Lotus
  infrastructure. It hides module-global setup, graph lookup, library matching,
  and shared layout queries behind a narrow interface.
- `TaintModel` is not the whole analysis by itself. Instead, it supplies one
  source of facts that the executor can mix with symbolic constraints and memory
  modeling when checking bug-specific conditions.

## Relation to the BOF Paper

The BOF work motivates the use of symbolic heaps, guarded memory reasoning, and
disjointness-oriented bug checking in this directory. The current Lotus code is
broader than that paper's presentation, though. It still uses the same core
ideas around symbolic state, guarded flow, and SMT-backed feasibility, but the
implementation now serves as a shared engine for more bug classes than the BOF
evaluation alone describes.

## Headers

Public interfaces live under `include/SymbolicExecution/`. New
contributors usually want to start with `SymbolicExecutionWrapper.h`,
`AnalysisDriver.h`, and `AnalysisState.h`, then read `ProgramVar.h`,
`PropertyValue.h`, `PropertySym.h`, `GVFGUtility.h`, and `TaintModel.h` to see
how symbolic values, utility shims, and taint specifications fit together.
