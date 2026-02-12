# Monotone Dataflow Analysis Framework

A layered framework for monotone dataflow analyses in LLVM IR.

## Directory Structure

```
include/Dataflow/Mono/
├── Core/                          # Core abstractions
│   ├── Problem.h                  # IntraMonoProblem & InterMonoProblem
│   ├── Domain.h                   # LLVMMonoAnalysisDomain
│   ├── CallStringContext.h        # CallStringCTX template
│   └── CallStringSolver.h        # Call-string interprocedural solver
├── Solver/                        # Fixpoint solvers
│   ├── IntraSolver.h              # Intraprocedural solver
│   └── InterSolver.h               # Interprocedural solver
├── Container/                     # Container utilities
│   ├── BitVectorSet.h             # Bit-vector optimized sets
│   └── Traits.h                   # Container abstractions
├── Support/                       # Support utilities
│   ├── Result.h                   # DataFlowResult structures
│   ├── MonoDebug.h                # Debugging utilities
│   └── Soundness.h                # Soundness configuration
└── Analyses/                      # Analysis implementations
    ├── Intra/                     # LiveVariables, ReachingDefinitions, etc.
    └── Inter/                      # TaintAnalysis, ConstantPropagation, etc.
```

## Quick Start

### Define an Analysis

```cpp
#include "Dataflow/Mono/Core/Problem.h"
#include "Dataflow/Mono/Solver/IntraSolver.h"

class MyAnalysis : public mono::IntraMonoProblem<mono::ValueSetDomain> {
public:
  std::set<llvm::Value*> normalFlow(llvm::Instruction *Inst, 
                                     const std::set<llvm::Value*> &In) override {
    // Transfer function
  }
  
  std::set<llvm::Value*> merge(const std::set<llvm::Value*> &Lhs,
                                const std::set<llvm::Value*> &Rhs) override {
    std::set<llvm::Value*> Result = Lhs;
    Result.insert(Rhs.begin(), Rhs.end());
    return Result;
  }
  
  bool equal_to(const std::set<llvm::Value*> &Lhs,
                const std::set<llvm::Value*> &Rhs) override {
    return Lhs == Rhs;
  }
  
  std::unordered_map<llvm::Instruction*, std::set<llvm::Value*>> 
  initialSeeds() override {
    return {};
  }
};
```

### Run the Analysis

```cpp
MyAnalysis Problem(EntryPoints);
mono::IntraMonoSolver<mono::ValueSetDomain> Solver(Problem);
Solver.solve();
auto Results = Solver.getInResults();
```

## Architecture

### Core (`Core/`)
- **`IntraMonoProblem`**: Base class with `normalFlow()`, `merge()`, `equal_to()`, `initialSeeds()`
- **`InterMonoProblem`**: Extends with `callFlow()`, `returnFlow()`, `callToRetFlow()`
- **`LLVMMonoAnalysisDomain`**: LLVM IR type definitions (`Instruction*` nodes, `Value*` facts)
- **`CallStringContext`**: Bounded call-string context representation
- **`CallStringSolver`**: Call-string based interprocedural solver engine

### Solvers (`Solver/`)
- **`IntraSolver`**: Worklist-based intraprocedural fixpoint solver
- **`InterSolver`**: Interprocedural solver with context sensitivity

### Containers (`Container/`)
- **`BitVectorSet`**: O(N/64) set operations for large universes (>100 elements)
- **`SetContainer`**: Standard `std::set` wrapper
- **`BitVectorContainer`**: `BitVectorSet` wrapper

### Analyses (`Analyses/`)
- **Intra**: LiveVariables, ReachingDefinitions, AvailableExpressions, ConstantPropagation, UninitVariables, Reachable
- **Inter**: TaintAnalysis, ConstantPropagation

## Design Principles

- **Separation of Concerns**: Clear boundaries between problem, solver, and utilities
- **Layered Architecture**: Minimal dependencies between layers
- **Composability**: Mix and match components
- **Performance**: Optimized containers and efficient solvers

## Migration

Old includes:
```cpp
#include "Dataflow/Mono/DataFlow.h"
#include "Dataflow/Mono/Solver/IntraMonoSolver.h"
```

New includes:
```cpp
#include "Dataflow/Mono/Core/Problem.h"
#include "Dataflow/Mono/Solver/IntraSolver.h"
```

## Further Reading

See `Core/Problem.h`, `Solver/IntraSolver.h`, and `Analyses/` for detailed documentation and examples.
