# UseTraceSSA

An LLVM analysis overlay for ordered use histories over existing SVFG value
flow. Public headers are in `include/IR/UseTraceSSA`, namespace
`lotus::usetracessa`.

## Representation

A use creates a history ψ; joins create history φ nodes; cycles represent
loops without unrolling. Each original SVFG definition has its own channel.
Local transfers leave the consumer's after-use port, preserving earlier uses.

`TemporalHistory` represents execution order once per CFG. Object identity is
metadata in `FlowEdge::objects` and `FlowNode::effects`, never a temporal node
or product-state dimension. Guarded effects keep separate event, certainty and
ObjectSet records supplied by upstream facts.

* Unknown ObjectSet is TOP; known empty is BOTTOM.
* Applicable events combine by bitwise union. Any applicable May effect or TOP
  guard makes the combined event May, retaining a no-effect alternative.
* Singleton points-to sets do not imply Must. Pointer analysis is unchanged.
* Generic taint queries permit different objects at different accesses.

## Queries

```cpp
auto q = queries::useAfterFree(graph);
q.memoryObject = object;
auto witness = QueryEngine(graph).run(q); // Fixed object throughout the path.
q.memoryObject.reset();
auto batch = QueryEngine(graph).runObjects({q, ObjectUniverse(candidates)});
auto status = batch.status(object);       // Found / NotFound / Unknown.
```

Both object modes use `(FlowNodeID, AutomatonState)` product states. Batches
attach finite bit masks to transitions and reachability facts: sequential steps
intersect masks; alternative paths union them. Call/return summaries match
`callSite` and propagate only newly discovered bits. Witnesses are reconstructed
lazily with the fixed-object engine. `UnknownResource` is a candidate sentinel
with no separate graph structure.

Queries also accept `contextLimit`: leaving it unset uses unbounded Dyck
summaries. A supplied k uses Saber's call-string limit semantics, including
k=0 for immediate context merging. The CLI exposes this as
`--context-limit=k`; omit the option for unbounded matching.

`DefectDetector` supplies double-free, use-after-free, memory-leak, file-leak,
taint and unchecked-use rules. Its resource scan searches all objects and sinks
symbolically, returning
one concrete witness and the accepted objects per sink. Found means a potential
witness in the supplied abstraction, not proven path feasibility. Missing models,
unsupported exceptional calls and exhausted budgets can make negatives Unknown.
Leak checks are candidate exit-path analyses. They track `malloc`/`calloc` with
`free` and `fopen` with `fclose` through root function exits. A pointer returned
from a root function is treated as escaped; ownership transfer through globals
and containers is not fully modeled. Direct release of an acquisition result is
recognized as a definite close/free.

## Build and use

```sh
cmake --build build --target lotus-ir-usetracessa
build/bin/lotus-ir-usetracessa input.bc --format=json
build/bin/lotus-ir-usetracessa input.bc --check=use-after-free
ctest --test-dir build -R 'UseTraceSSA|usetracessa_' --output-on-failure
```

The native importer uses upstream SVFG object facts and LLVM site ordering.
Direct internal calls, recursion and multiple callers share temporal ports.
Taint policies and complete external-call semantics require client models.
JSON schema 2 serializes guarded effects, using `null` for TOP and `[]` for
BOTTOM. The API and CLI expose graph, query and mask-operation statistics.
