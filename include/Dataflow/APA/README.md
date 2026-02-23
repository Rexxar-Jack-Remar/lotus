# APA Dataflow (Elimination / State Elimination)

Header layout for the APA-based intraprocedural dataflow solver. Structure mirrors `Dataflow/Mono`.

## Directory structure

```
include/Dataflow/APA/
├── DataFlow.h                     # Umbrella header (include this for full API)
├── Core/                          # Core abstractions (header-only)
│   ├── Framework.h                # IntraEliminationProblem, IntraReducibleEliminationProblem
│   ├── PathExpression.h           # Path-expression AST (PathExprFactory, etc.)
│   └── Options.h                  # EliminationMethod, EliminationOptions
├── Support/                       # Support utilities (header-only)
│   └── Result.h                   # DataFlowResultT
├── Solver/                        # Solver (header-only)
│   └── IntraEliminationSolver.h
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

## Conventions

- **Legacy identifiers:** include guards and several type names still use
  `Elimination` / `DATAFLOW_ELIMINATION_*` for source compatibility (e.g.
  `DATAFLOW_ELIMINATION_CORE_FRAMEWORK_H_`).

## References

See `lib/Dataflow/APA/README.md` for full reference list, including:

- Classical elimination-based dataflow: Aho, Sethi, Ullman (Dragon Book); Muchnik
- Algebraic Program Analysis: Reps & Kincaid papers (2014-2018)
