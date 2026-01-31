# Newtonian Program Analysis (NPA)

NPA implements compositional, Newton-style program analysis over ω-continuous semirings.

## Structure

```
include/Dataflow/NPA/
├── NPA.h                      # Framework core (Exp0/Exp1, Kleene/Newton, LCFL/TOPLAS 2016)
├── Domains/                   # Abstract domains & problem specs
│   ├── BitVectorDomain.h
│   ├── BitVectorInfo.h
│   ├── GenKillDomain.h
│   ├── TaintTransferDomain.h
│   └── TensorProductDomain.h
└── Analyses/                  # Solvers & analyses
    ├── BitVectorSolver.h
    ├── InterproceduralEngine.h
    ├── Intraprocedural/
    │   ├── ReachingDefinitions.h
    │   └── ReachableBlocks.h
    └── Interprocedural/
        ├── InterproceduralRD.h
        └── InterproceduralTaint.h
```

## Usage

- **Intraprocedural**: `BitVectorSolver` + `BitVectorInfo` implementation.
- **Interprocedural**: `InterproceduralEngine<Domain, Analysis>` + analysis policy.
