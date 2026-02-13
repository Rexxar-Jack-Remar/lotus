# Elimination Dataflow (State Elimination)

Header layout for the elimination-based intraprocedural dataflow solver. Structure mirrors `Dataflow/Mono`.

## Directory structure

```
include/Dataflow/Elimination/
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

- **Single include:** `#include "Dataflow/Elimination/DataFlow.h"` (pulls in Core, Support, Solver, LLVM).
- **Minimal:** include only what you need, e.g. `Core/Framework.h`, `Support/Result.h`, `Solver/IntraEliminationSolver.h`, `LLVM/LLVMEliminationProblem.h`.
- **Passes:** `#include "Dataflow/Elimination/Passes/EliminationPasses.h"` for LLVM pass classes.

## Conventions

- **Include guards:** `DATAFLOW_ELIMINATION_<SUBDIR>_<FILE>_H_` (e.g. `DATAFLOW_ELIMINATION_CORE_FRAMEWORK_H_`).
