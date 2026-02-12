# Sparse Value-Flow Graph (SVFG)

Production-ready implementation of the Sparse Value-Flow Graph (SVFG) for whole-program value-flow analysis. The SVFG provides a sparse representation of value flows, enabling efficient interprocedural analysis with Memory SSA integration.

**This implementation uses [AserPTA](lib/Alias/AserPTA) as its points-to analysis engine** to compute points-to sets for building value-flow edges and Memory SSA.

## Key Features

- **Sparse value-flow representation** for whole-program analysis
- **Memory SSA integration** for precise alias tracking
- **Interprocedural value-flow** through calls and returns
- **AserPTA-based points-to analysis** (default pointer analysis engine)
- **On-the-fly call graph refinement** for demand-driven analyses

## Usage

```cpp
#include "IR/SVFG/SVFG.h"
#include "IR/SVFG/SVFGBuilder.h"

// Build SVFG from ICFG
SVFGBuilder builder;
SVFG* svfg = builder.build(icfg);

// Query value flows
SVFGNode* def = svfg->getDef(instruction);
SVFGNodeSet succs = svfg->getSuccs(node);
bool hasPath = svfg->hasPath(src, dst);
```

## Configuration

```cpp
SVFGBuilderConfig config;
config.buildMSSA = true;
config.resolveIndirectCalls = true;
config.solverType = SVFGBuilderConfig::SolverType::Andersen;
config.memModelType = SVFGBuilderConfig::MemModelType::FieldSensitive;

SVFGBuilder builder(config);
SVFG* svfg = builder.build(icfg);
```

## Components

- **`SVFG`**: Main graph class
- **`SVFGBuilder`**: Constructs SVFG from ICFG using **AserPTA** for points-to analysis
- **Node types**: Stmt, MSSA (Mu/Chi/Phi), Formal/Actual Parm/Ret, Addr
- **Edge types**: Intra-procedural (Copy, Load, Store, GEP, etc.), Memory SSA, Interprocedural (Call/Ret)

## Dependencies

- CanaryICFG, AserPTA, LLVM Core/Support

## References

- Yulei Sui, Ding Ye, Jingling Xue. "Detecting Memory Leaks Statically with Full-Sparse Value-Flow Analysis". TSE'14.
- Yulei Sui, Jingling Xue. "On-Demand Strong Update Analysis via Value-Flow Refinement". FSE'16.
