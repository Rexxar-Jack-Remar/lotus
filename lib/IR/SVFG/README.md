# Sparse Value-Flow Graph (SVFG)

Production-ready implementation of the Sparse Value-Flow Graph (SVFG) for whole-program value-flow analysis. The SVFG provides a sparse representation of value flows, enabling efficient interprocedural analysis with Memory SSA integration.

**This implementation uses [AserPTA](lib/Alias/AserPTA) as its points-to analysis engine** to compute points-to sets for building value-flow edges and Memory SSA.

## What is SVFG?

SVFG represents value flow in a program as a directed graph where:
- **Nodes** represent definitions (assignments, loads, stores, parameters)
- **Edges** represent value-flow relationships (def-use chains)
- **Memory SSA** tracks memory versions through MU/CHI nodes

### Example

```c
int x = 5;           // AddrNode (defines x)
int *p = &x;         // CopyNode (p = &x)
int y = *p;          // LoadNode with LoadMu (reads memory)
*p = 10;             // StoreNode with StoreChi (writes memory)
```

SVFG representation:
```
AddrNode(x) --> CopyNode(p) --> LoadNode(y)
                                    ^
                                    |
                                LoadMu(mem_1)
                                    ^
                                    |
StoreNode(*p) --> StoreChi(mem_1 -> mem_2)
```

## Key Features

- **Sparse value-flow representation** for whole-program analysis
  - Only tracks values that flow through memory or across functions
  - Omits purely local computations (e.g., x = a + b)
  - Reduces graph size while preserving essential def-use information

- **Memory SSA integration** for precise alias tracking
  - Extends SSA to memory locations using MU (use) and CHI (def) nodes
  - Each memory region has versioned definitions (e.g., mem_1, mem_2)
  - Enables precise tracking of memory dependencies

- **Interprocedural value-flow** through calls and returns
  - FormalIN/FormalOUT: Parameter/return value definitions in callee
  - ActualIN/ActualOUT: Argument/return value uses at call site
  - Call/Return edges connect caller and callee contexts

- **AserPTA-based points-to analysis** (default pointer analysis engine)
  - Guards memory edges with points-to sets (which objects may be accessed)
  - Supports field-sensitive and context-sensitive analysis
  - Enables precise memory dependence tracking

- **On-the-fly call graph refinement** for demand-driven analyses
  - Indirect call edges can be materialized on-demand
  - Supports incremental analysis and query-driven exploration

## DDA-Oriented Design Notes

Lotus DDA (`lib/Alias/DDA`) consumes SVFG with a few strict assumptions:

- **Object-ID namespace is disjoint from SVFG node IDs**.
  - Object IDs represent abstract memory objects in edge guards.
  - SVFG node IDs represent program-value/memory SSA nodes.
- **Indirect/memory edges carry guard sets of object IDs**.
  - Empty guard means unconstrained flow.
  - Unknown object ID (wildcard) means conservative may-alias-anything.
- **Object metadata is available via SVFG**.
  - `isConstant`, `isUnknown`, `isFunction`, `isHeap`, etc.
  - DDA uses this to prune immutable objects and keep fallback sound.
- **Indirect callsite indices are maintained**.
  - DDA can discover function-pointer targets and add call/ret edges on demand.
  - Reverse mapping (callee -> invoking indirect callsites) is tracked too.

## Build Pipeline

`SVFGBuilder` executes (conceptually) in these phases:

1. **Pointer analysis bootstrap** (AserPTA) and object-ID mapping.
2. **Node construction**:
   - Top-level statement nodes (`Addr/Copy/Load/Store/Gep/Phi/...`)
   - Inter-procedural nodes (`Actual*/Formal*`)
   - Memory SSA nodes (`Mu/Chi/Phi/EntryChi/CallMu/CallChi/...`)
3. **Edge construction**:
   - Direct value-flow edges (copy/gep/phi/param/ret etc.)
   - Guarded indirect/memory edges (object-sensitive)
4. **Inter-procedural refinement**:
   - Direct-call edges always connected
   - Indirect-call edges optionally deferred for on-the-fly DDA refinement
5. **Memory SSA linking** and optional optimization/update passes.

## Unknown Object Semantics

Unknown object is created lazily and used as a **wildcard** object ID:

- If points-to information is unavailable/ambiguous, edges may carry unknown.
- DDA out-of-budget fallback can use unknown when precise object IDs are absent.
- Unknown preserves soundness but can reduce precision.

## Integration Contract (SVFG <-> DDA)

When modifying SVFG, keep these contracts stable:

- `SVFG::getObjectValue(objId)` and `SVFG::getObjectInfo(objId)` must remain valid.
- `SVFGBuilder::getObjectIdsForValue(value)` should return PTA-backed IDs
  compatible with edge guard IDs.
- `SVFG::getIndCallSites`, `getConnectedCallees`, and callsite-ID mappings must
  stay consistent when edges are added on-the-fly.
- Guarded edges should use the same unknown-object convention as fallback paths.

## Usage

### Basic SVFG Construction

```cpp
#include "IR/SVFG/SVFG.h"
#include "IR/SVFG/SVFGBuilder.h"
#include "IR/ICFG/ICFG.h"

// Create ICFG from LLVM module
ICFG *icfg = ICFG::createICFG(module);

// Build SVFG with default configuration
SVFGBuilder builder;
SVFG* svfg = builder.build(icfg);

// Query value flows
SVFGNode* def = svfg->getDef(instruction);
SVFGNodeSet succs = svfg->getSuccs(node);
bool hasPath = svfg->hasPath(src, dst);
```

### Advanced Configuration

```cpp
SVFGBuilderConfig config;
config.buildMSSA = true;                    // Enable Memory SSA
config.resolveIndirectCalls = true;         // Resolve indirect calls via PTA
config.solverType = SVFGBuilderConfig::SolverType::Andersen;
config.memModelType = SVFGBuilderConfig::MemModelType::FieldSensitive;

SVFGBuilder builder(config);
SVFG* svfg = builder.build(icfg);
```

### Querying SVFG

```cpp
// Get definition node for an instruction
if (SVFGNode *node = svfg->getDef(inst)) {
    // Traverse successors (uses)
    for (SVFGEdge *edge : node->getOutEdges()) {
        SVFGNode *succ = edge->getDstNode();
        // Process successor
    }
}

// Check reachability
if (svfg->hasPath(srcNode, dstNode)) {
    // Value flows from src to dst
}

// Get points-to set for a memory node
if (auto *load = llvm::dyn_cast<LoadSVFGNode>(node)) {
    const SVFGNodeBS *pts = load->getPointsTo();
    for (uint32_t objId : *pts) {
        const llvm::Value *obj = svfg->getObjectValue(objId);
        // Process pointed-to object
    }
}
```

### Traversing Memory SSA

```cpp
// Find all memory definitions reaching a load
if (auto *load = llvm::dyn_cast<LoadSVFGNode>(node)) {
    for (SVFGEdge *edge : load->getInEdges()) {
        if (auto *muEdge = llvm::dyn_cast<MUEdge>(edge)) {
            SVFGNode *def = muEdge->getSrcNode();
            // def is a memory definition (StoreChi, CallChi, etc.)
        }
    }
}

// Find all uses of a memory definition
if (auto *store = llvm::dyn_cast<StoreSVFGNode>(node)) {
    // Get the StoreChi node
    for (SVFGEdge *edge : store->getOutEdges()) {
        if (auto *chiEdge = llvm::dyn_cast<CHIEdge>(edge)) {
            SVFGNode *chi = chiEdge->getDstNode();
            // chi is a StoreChi node
            for (SVFGEdge *useEdge : chi->getOutEdges()) {
                SVFGNode *use = useEdge->getDstNode();
                // use is a memory use (LoadMu, CallMu, etc.)
            }
        }
    }
}
```

## Components

### Core Classes

- **`SVFG`**: Main graph class
  - Manages nodes and edges
  - Provides query interface (getDef, getSuccs, hasPath, etc.)
  - Maintains object metadata for DDA
  - Tracks indirect call sites for on-the-fly refinement

- **`SVFGBuilder`**: Constructs SVFG from ICFG using **AserPTA** for points-to analysis
  - Phases: PTA bootstrap → Node construction → Edge construction → Memory SSA
  - Configurable solver (Andersen, WavePropagation, etc.)
  - Configurable memory model (field-sensitive, field-insensitive)

### Node Types

- **Statement nodes**: Addr, Copy, Load, Store, Gep, BinaryOp, Cmp, Branch
  - Represent direct value definitions from LLVM instructions
  - Example: `x = y` creates a CopySVFGNode

- **PHI nodes**: IntraPhi, InterPhi, MemPhi, MemInterPhi
  - Merge values at control-flow join points
  - IntraPhi: Within a function (loop/branch merge)
  - InterPhi: Across functions (call/return merge)

- **Memory SSA nodes**: FormalIN/OUT, ActualIN/OUT, LoadMu, StoreChi, CallMu, CallChi
  - Track memory versions through MU (use) and CHI (def)
  - Enable precise memory dependence analysis

- **Parameter nodes**: FormalParm, ActualParm, FormalRet, ActualRet
  - Connect caller and callee contexts
  - Enable interprocedural value-flow tracking

### Edge Types

- **Intra-procedural edges**: Copy, Load, Store, GEP, Phi, BinaryOp
  - Connect nodes within the same function
  - Represent direct value-flow (def-use chains)

- **Call/Return edges**: CallDir, CallInd, RetDir, RetInd
  - Connect actual arguments to formal parameters
  - Connect return values to call sites
  - Direct: Statically resolved calls
  - Indirect: Function pointer calls (resolved via PTA)

- **Memory edges**: MU, CHI, MHP
  - MU: Memory use (read) edge
  - CHI: Memory def (write) edge
  - MHP: May-happen-in-parallel (threading)

## Dependencies

- CanaryICFG, AserPTA, LLVM Core/Support

## References

- Yulei Sui, Ding Ye, Jingling Xue. "Detecting Memory Leaks Statically with Full-Sparse Value-Flow Analysis". TSE'14.
- Yulei Sui, Jingling Xue. "On-Demand Strong Update Analysis via Value-Flow Refinement". FSE'16.
