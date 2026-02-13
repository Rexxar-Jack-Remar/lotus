# WPDS-based Interprocedural Dataflow Analysis Framework

A framework for interprocedural dataflow analyses using Weighted Pushdown Systems (WPDS) in LLVM IR.

## Overview

WPDS is a solver for distributive data-flow problems, very similar to IDE (Interprocedural Distributive Environment). Instead of building an exploded super-graph using user-provided flow and edge functions, a weighted pushdown system is built whose rules are drawn from the user's analysis description. The analysis problem is solved using a stack automaton obtained by the post* or pre* algorithm using the WALi library (`lib/Solvers/WPDS`).

## Directory Structure

```
lib/Dataflow/WPDS/
├── Core/                          # Core abstractions
│   ├── DataFlowFacts.cpp         # Fact domain implementation
│   └── GenKillTransformer.cpp   # Semiring weight implementation
├── Solver/                       # Fixpoint solvers
│   └── InterProceduralDataFlowEngine.cpp  # WPDS solver engine
├── Clients/                       # Client analysis implementations
│   ├── WPDSTaintAnalysis.cpp
│   ├── WPDSConstantPropagation.cpp
│   ├── WPDSLivenessAnalysis.cpp
│   └── WPDSUninitializedVariables.cpp
├── CMakeLists.txt
└── README.md
```

## Quick Start

To write a WPDS analysis, you essentially write an IDE analysis. The framework provides:

1. **Define flow functions** using `GenKillTransformer` for gen/kill-style dataflow
2. **Run the analysis** using `InterProceduralDataFlowEngine`

```cpp
#include "Dataflow/WPDS/InterProceduralDataFlow.h"

wpds::InterProceduralDataFlowEngine Engine;
auto Result = Engine.runForwardAnalysis(
    M,
    [](Instruction* inst) { return /* transformer */; },
    {/* initial facts */}
);
```

## References

- Reps, Schwoon, Jha, Melski: "Weighted pushdown systems and their application to interprocedural dataflow analysis" (SAS 2005)
- Lal, Reps: "Improving Pushdown System Model Checking" (CAV 2006)
- Lal, Reps, Balakrishnan: "Extended Weighted Pushdown Systems" (CAV 2005)
