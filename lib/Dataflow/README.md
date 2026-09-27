# Dataflow Analysis Library

This library provides multiple interprocedural and intraprocedural dataflow solvers.

| Subdir | Purpose |
|--------|---------|
| **APA** | Algebraic program analysis infrastructure for dataflow problems. |
| **DemandAPA** | Demand-driven, query-oriented APA over Boolean programs (BDD-backed). |
| **IFDS/IDE** | Interprocedural tabulation. IFDS: reachability / may-analyses (taint, uninit). IDE: value-carrying facts (constant prop, typestate). |
| **Mono** | Monotone frameworks. Intra- and interprocedural; call-string context. Constant prop, liveness, taint. |
| **NPA** | Newtonian program analysis over ω-continuous semirings. Generalizes lattices. TOPLAS 2016 / LCFL support. |
| **VASCO** | Value-sensitive context-scheduling for interprocedural dataflow. |
| **WPDS** | Weighted pushdown systems (WALi). Equivalent to IDE; uses post*/pre* on a pushdown rule set. Constant prop, taint, liveness, uninit, etc. |
| **Tooling** | Shared command-line drivers for the solvers above. |

See each subdirectory’s README for details and headers.
