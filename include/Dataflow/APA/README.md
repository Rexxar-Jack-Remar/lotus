# APA Dataflow (Elimination / State Elimination)

Header layout for the APA-based intraprocedural dataflow solver. Structure mirrors `Dataflow/Mono`.

## Directory structure

```
include/Dataflow/APA/
├── DataFlow.h                     # Umbrella header (include this for full API)
├── Core/                          # Core abstractions (header-only)
│   ├── Framework.h                # IntraEliminationProblem, IntraReducibleEliminationProblem
│   ├── PathExpression.h           # Transfer-expression AST (PathExprFactory, etc.)
│   └── Options.h                  # EliminationMethod, EliminationOptions
├── Support/                       # Support utilities (header-only)
│   └── Result.h                   # DataFlowResultT
├── Solver/                        # Solver (header-only)
│   ├── IntraEliminationSolver.h   # Public facade / method dispatch
│   ├── EngineCommon.h             # Shared ADT / CFG / evaluation helpers
│   ├── StateEliminationEngine.h   # Generic O(n^3) elimination engine
│   ├── ADTSimpleEngine.h          # ADT "simple" engine
│   └── ADTDelayedEngine.h         # ADT "delayed" engine
├── LLVM/                          # LLVM IR adapter (header-only)
│   ├── LLVMEliminationProblem.h
│   └── LLVMReverseEliminationProblem.h
├── Analyses/                      # Analysis headers (implementations in lib/)
│   └── Intra/                     # Intra*.h — Reachable, ConstantPropagation, etc.
└── Passes/                        # LLVM pass wrappers
    └── EliminationPasses.h
```

## Quick include guide

- **Single include:** `#include "Dataflow/APA/DataFlow.h"` (pulls in Core, Support, Solver, LLVM).
- **Minimal:** include only what you need, e.g. `Core/Framework.h`, `Support/Result.h`, `Solver/IntraEliminationSolver.h`, `LLVM/LLVMEliminationProblem.h`.
- **Passes:** `#include "Dataflow/APA/Passes/EliminationPasses.h"` for LLVM pass classes.
- **Internal engine headers:** `Solver/EngineCommon.h` and the three `*Engine.h`
  headers are solver internals; downstream analyses should normally include only
  `Solver/IntraEliminationSolver.h`.

## Conventions

- **Legacy identifiers:** include guards and several type names still use
  `Elimination` / `DATAFLOW_ELIMINATION_*` for source compatibility (e.g.
  `DATAFLOW_ELIMINATION_CORE_FRAMEWORK_H_`).

## Path-expression terminology

- `Core/PathExpression.h` is APA-specific. It models transfer-function
  expressions (`Atom`, `Union`, `Concat`, `Star`) that the solver later
  evaluates over a dataflow lattice.
- `Utils/Algorithms/PathExpressions/` is a separate utility library for
  Tarjan-style regex construction over generic labeled graphs.
- The two components are related in spirit, but they serve different roles and
  are not interchangeable APIs.

## Solver architecture

- `IntraEliminationSolver.h` owns the shared context and performs method
  dispatch.
- `EngineCommon.h` centralizes shared graph metadata, ADT construction,
  reducibility checks, and expression evaluation.
- `StateEliminationEngine.h` is the always-available baseline algorithm.
- `ADTSimpleEngine.h` and `ADTDelayedEngine.h` implement the two paper-style
  reducible-graph variants.

## References

See `lib/Dataflow/APA/README.md` for full reference list, including:

- Classical elimination-based dataflow: Aho, Sethi, Ullman (Dragon Book); Muchnik
- Algebraic Program Analysis: Reps & Kincaid papers (2014-2018)
