# Demand-Driven Pointer Analysis (DDA)

Value-flow-based demand-driven pointer analysis following **SVF's FlowDDA / DDAVFSolver** design (FSE'16, TSE'18). It answers points-to and alias queries on demand by backward traversal on the Sparse Value-Flow Graph (SVFG), instead of computing whole-program points-to up front.

## Scope In Lotus

- `FlowDDA`: flow-sensitive, context-insensitive demand solver.
- `ContextDDA`: flow-sensitive, call-string context-sensitive solver.
- `DDAVFSolver`: shared generic backward solver used by both modes.
- `DDAPass`: driver that selects solver mode and query client.
- `DDAClient` variants: query selection strategy (`All`, `Funptr`, `Alias`).

## Core Query Algorithm

Given a queried pointer value `p`:

1. Map `p` to its defining SVFG node and initialize a DPM state `(cur, loc)`.
2. Run `findPT(dpm)` in `DDAVFSolver`:
   - Dispatch by SVFG node kind (`Addr`, `Copy/Phi`, `Gep`, `Load`, `Store`, memory nodes).
   - Traverse direct/indirect incoming value-flow edges as required.
   - Recurse on predecessor DPMs; union returned points-to sets.
3. Cache discovered facts in:
   - top-level pointer cache (`dpmToTLPtsMap_`), and
   - address-taken/memory cache (`dpmToADPtsMap_`).
4. When new facts appear for a DPM, trigger `reCompute` on dependents so
   previously visited states can be refined.
5. Stop when the query reaches a fixpoint or hits budget.

## Out-Of-Budget Fallback

Lotus follows SVF's design of conservative fallback when a demand query exceeds
the step budget:

- Primary fallback source: pointer-analysis object sets from `SVFGBuilder`
  (`AserPTA`-backed in Lotus).
- Candidate fallback values are collected from:
  - query-location value (`dpm.getLoc()->getValue()`), and
  - object-mapped value (`svfg->getObjectValue(dpm.getCurNodeID())`).
- If no object ID can be recovered, fallback inserts the SVFG unknown object.

This guarantees a non-empty conservative result whenever unknown object exists,
while still preferring precise PTA-derived IDs.

## Context Sensitivity

`ContextDDA` extends the same transfer logic with call-string constraints:

- Call/return edges update or match context IDs.
- Recursive callsites are treated context-insensitively where needed.
- Context-insensitive edge set is initialized from recursion/value-flow cycles.
- Out-of-budget in `ContextDDA` downgrades to conservative object-level fallback.

## SVFG Assumptions And Invariants

The DDA implementation assumes:

- Each pointer query can be mapped to an SVFG def node.
- Direct and indirect SVFG edge classes are consistent with `isDirectVFGEdge`
  and `isIndirectVFGEdge`, plus Lotus-specific extensions.
- Indirect edge guards (`edge->getPointsTo()`) represent object-sensitive flow.
- Constant/immutable objects are identified (`svfg->isConstantObject`) and
  skipped during indirect memory propagation.

## Key Files

- `include/Alias/DDA/DDAVFSolver.h`: generic solver algorithm and caches.
- `include/Alias/DDA/FlowDDA.h`, `lib/Alias/DDA/FlowDDA.cpp`: flow-sensitive mode.
- `include/Alias/DDA/ContextDDA.h`, `lib/Alias/DDA/ContextDDA.cpp`: context mode.
- `include/Alias/DDA/DDAClient.h`: query clients and candidate collection.
- `include/Alias/DDA/DDAPass.h`, `lib/Alias/DDA/DDAPass.cpp`: orchestration.
- `include/Alias/DDA/DDAStat.h`, `lib/Alias/DDA/DDAStat.cpp`: solver statistics.


## References

- Yulei Sui, Jingling Xue. "On-Demand Strong Update Analysis via Value-Flow Refinement". FSE'16.
- Yulei Sui, Jingling Xue. "Value-Flow-Based Demand-Driven Pointer Analysis for C and C++". TSE'18.
