# CFG: Control Flow Graph Analysis Utilities

CFG provides control flow graph analysis utilities for reachability and structural analysis of LLVM IR functions. These components are used to pre-compute structural information for subsequent dataflow analyses.

## Key Features

- **Reachability analysis**: Determine if basic blocks or instructions are reachable from each other
- **Dominator analysis**: Compute dominator and post-dominator trees
- **Topological ordering**: Generate topological orderings of CFG nodes
- **Code metrics**: Compute complexity metrics (cyclomatic, NPath, loop nesting)

## Components

### `CFGReachability.cpp`
Provides reachability analysis for basic blocks and instructions within a function's control flow graph. Uses BFS-based analysis with caching for efficient queries.

**Usage:**
```cpp
#include "Analysis/CFG/CFGReachability.h"

CFGReachability reach(&F);
bool reachable = reach.reachable(FromBB, ToBB);
bool reachable = reach.reachable(FromInst, ToInst);
```

### `Dominator.cpp`
Unified interface for accessing both dominator and post-dominator information. Wraps LLVM's DominatorTree and PostDominatorTree.

### `TopologicalOrder.cpp`
Constructs topological order of a CFG and identifies back edges. Provides iterators for forward and reverse topological traversal.

**Usage:**
```cpp
TopologicalOrder topo;
topo.runOnFunction(F);
for (auto *BB : topo) {
  // Process in topological order
}
bool isBackEdge = topo.isBackEdge(srcBB, dstBB);
```

### `WeakTopologicalOrder.cpp` / `WeakTopologicalOrder2.cpp`
Weak topological ordering algorithms for CFG traversal.

### `CodeMetrics.h`
Computes code complexity metrics:
- **Cyclomatic complexity**: Number of independent paths through code
- **NPath complexity**: Total number of unique execution paths
- **Loop metrics**: Loop count and maximum nesting depth

## Typical Use Cases

- Decide whether a basic block is reachable from another
- Compute loop nests and dominance relationships
- Pre-compute structural information used by subsequent dataflow analyses
- Analyze code complexity and structure

## Dependencies

- **LLVM**: Core IR, CFG utilities, LoopInfo, DominatorTree

## See Also

- Headers: `include/Analysis/CFG/`
- Documentation: `docs/source/analysis/cfg.rst`
