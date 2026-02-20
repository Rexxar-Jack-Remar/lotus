# Sparse Value-Flow Graph (SVFG)

Production-ready implementation of the Sparse Value-Flow Graph (SVFG) for whole-program value-flow analysis. The SVFG provides a sparse representation of value flows, enabling efficient interprocedural analysis with Memory SSA integration.

**This implementation uses [AserPTA](lib/Alias/AserPTA) as its points-to analysis engine** to compute points-to sets for building value-flow edges and Memory SSA.

## Key Features

- **Sparse value-flow representation** for whole-program analysis
- **Memory SSA integration** for precise alias tracking
- **Interprocedural value-flow** through calls and returns
- **AserPTA-based points-to analysis** (default pointer analysis engine)
- **On-the-fly call graph refinement** for demand-driven analyses

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
