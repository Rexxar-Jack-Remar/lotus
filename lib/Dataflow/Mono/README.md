# Monotone Dataflow Analysis

Monotone dataflow analysis frameworks for intraprocedural and interprocedural analysis.

**Layout (aligned with NPA):**

- **Solver/** — Call-string interprocedural engine and InterMonoSolver
- **Analyses/Intraprocedural/** — Constant propagation, uninit variables, liveness, reachability
- **Analyses/Interprocedural/** — Interprocedural taint analysis

**Headers (include/Dataflow/Mono/):** `DataFlow.h` (umbrella), `DataFlowResult.h`, `MonoFramework.h`; `Solver/` and `Analyses/Intraprocedural|Interprocedural/` mirror the source layout.
