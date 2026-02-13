# WPDS-based Interprocedural Dataflow Analysis Framework

A framework for interprocedural dataflow analyses using Weighted Pushdown Systems (WPDS) in LLVM IR.

## Directory Structure

```
include/Dataflow/WPDS/
├── Core/                          # Core abstractions
│   ├── DataFlowFacts.h            # Fact domain (set of facts / environment)
│   ├── GenKillTransformer.h       # Semiring weight (gen/kill + relational flow)
│   └── ExplodedWPDSBuilder.h      # Builder for exploded supergraph encoding
├── Solver/                        # Solver engine
│   └── InterProceduralDataFlowEngine.h  # WPDS solver, runs GPR algorithm
├── Clients/                       # Client analysis implementations
│   ├── WPDSTaintAnalysis.h
│   ├── WPDSConstantPropagation.h
│   ├── WPDSLivenessAnalysis.h
│   └── WPDSUninitializedVariables.h
├── Container/                     # Container utilities (placeholder)
├── Support/                       # Support utilities (placeholder)
└── InterProceduralDataFlow.h     # Umbrella header

lib/Dataflow/WPDS/
├── Core/
│   ├── DataFlowFacts.cpp
│   └── GenKillTransformer.cpp
├── Solver/
│   └── InterProceduralDataFlowEngine.cpp
├── Clients/
│   ├── WPDSTaintAnalysis.cpp
│   ├── WPDSConstantPropagation.cpp
│   ├── WPDSLivenessAnalysis.cpp
│   └── WPDSUninitializedVariables.cpp
└── CMakeLists.txt
```

## Architecture

### Core (`Core/`)
- **`DataFlowFacts`**: Fact domain representing a set of LLVM Values with set operations (union, intersect, diff)
- **`GenKillTransformer`**: Semiring weight implementing gen/kill-style flow functions for WPDS
- **`ExplodedWPDSBuilder`**: Template for building the paper's exploded supergraph encoding

### Solver (`Solver/`)
- **`InterProceduralDataFlowEngine`**: Encodes program supergraph as WPDS, runs forward/backward saturation (GPR), extracts results

### Clients (`Clients/`)
Pre-built analyses using the WPDS framework:
- **`WPDSTaintAnalysis`**: Taint analysis
- **`WPDSConstantPropagation`**: Constant propagation (values NOT in set are constant)
- **`WPDSLivenessAnalysis`**: Live variable analysis
- **`WPDSUninitializedVariables`**: Uninitialized variable detection

## Quick Start

```cpp
#include "Dataflow/WPDS/InterProceduralDataFlow.h"

class MyAnalysis {
public:
    GenKillTransformer* createTransformer(llvm::Instruction* inst) {
        // Define gen/kill flow
    }
};

wpds::InterProceduralDataFlowEngine Engine;
auto Result = Engine.runForwardAnalysis(
    M,
    [](Instruction* inst) { return /* transformer */; },
    {/* initial facts */}
);
```

## References

- Reps, Schwoon, Jha: "Weighted Pushdown Systems and their Application to Interprocedural Dataflow Analysis" (SAS 2005)
- Lal, Reps: "Improving Pushdown System Model Checking" (CAV 2006)
- Lal, Reps, Balakrishnan: "Extended Weighted Pushdown Systems" (CAV 2005)

## Migration

Old includes:
```cpp
#include "Dataflow/WPDS/DataFlowFacts.h"
#include "Dataflow/WPDS/GenKillTransformer.h"
#include "Dataflow/WPDS/InterProceduralDataFlowEngine.h"
```

New includes:
```cpp
#include "Dataflow/WPDS/Core/DataFlowFacts.h"
#include "Dataflow/WPDS/Core/GenKillTransformer.h"
#include "Dataflow/WPDS/Solver/InterProceduralDataFlowEngine.h"
```

Or use the umbrella header:
```cpp
#include "Dataflow/WPDS/InterProceduralDataFlow.h"
```
