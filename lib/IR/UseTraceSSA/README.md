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

Native construction requires an explicit `NativeHistoryMode`. The CLI chooses
it from `--check` before creating the graph. `Full` is an explicit general
overlay for generic queries and graph dumps; rebuild when changing properties.

| Property | Recorded resource events | Candidate-object sources |
|---|---|---|
| Double-free | Allocate, Release | Allocate / Release |
| Use-after-free | Allocate, Release, Dereference | Allocate / Release |
| Memory-leak | Allocate, Release, Escape, Exit | Allocate |
| File-leak | Open, Close, Escape, Exit | Open |

Allocation resets are retained for double-free and use-after-free. With finite
source guards, unrelated accesses/effects are omitted and candidate enumeration
is limited to source objects. TOP source guards conservatively keep all relevant
objects. TOP access guards remain TOP, preserving their May semantics; the
finite property universe is recorded separately (`resource_universe` in JSON).
Event or topology edits invalidate this derived universe.

Intrinsic no-effect sites and unrelated modeled resource calls are omitted.
Internal and unknown calls, CFG branches, loops, and normal return ports remain
available for context-sensitive control flow. Missing required native events
produce an empty resource graph with an incompleteness issue, not a safety proof.

```cpp
auto q = queries::useAfterFree(graph);
q.memoryObject = object;
auto witness = QueryEngine(graph).run(q); // Fixed object throughout the path.
auto sinks = QueryEngine(graph).runToSinks(q); // One witness per reached sink.
q.memoryObject.reset();
auto batch = QueryEngine(graph).runObjects({q, ObjectUniverse(candidates)});
auto status = batch.status(object);       // Found / NotFound / Unknown.
```

Both object modes use `(FlowNodeID, AutomatonState)` product states. Batches
attach finite bit masks to transitions and reachability facts: sequential steps
intersect masks; alternative paths union them. Call/return summaries match
`callSite` and propagate only newly discovered bits. Product transition labels
are compiled once and reused by later mask deltas. `runToSinks()` shares product
construction and context solving across every sink of a fixed-object or generic
query. Its `complete` flag distinguishes exhaustive enumeration from a partial
result caused by budgets or incomplete modeling. `UnknownResource` is a
candidate sentinel with no separate graph structure.

Internally, query facts share immutable object masks, including canonical empty
and universal masks. Public results retain ordinary `ObjectMask` bit vectors.
Set `ObjectBatchQuery::retainWitnesses` to retain provenance for bounded-context,
context-insensitive, or call-free queries. When `batch.hasWitnesses()` is true,
`batch.witness(sink, object)` follows a shared proof chain without another search.
Proofs are shared across objects; only acceptance records carry object masks.

Queries also accept `contextLimit`: leaving it unset uses unbounded Dyck
summaries. A supplied k uses Saber's call-string limit semantics, including
k=0 for immediate context merging. The CLI exposes this as
`--context-limit=k`; omit the option for unbounded matching.

`DefectDetector` supplies double-free, use-after-free, memory-leak, file-leak,
taint and unchecked-use rules. Its resource scan searches all objects and sinks
symbolically, returning one concrete witness and the accepted objects per sink.
Bounded-context scans recover witnesses from the symbolic traversal. Unbounded
Dyck scans group witness queries by representative object and use one
`runToSinks()` traversal per group. The single-result `run()` stops reconstructing witnesses
after the first finding. Found means a potential
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
Instruction labels are captured in one module printing pass, preserving LLVM
formatting and source locations without scanning module globals for every label.
Taint policies and complete external-call semantics require client models.
JSON schema 2 serializes guarded effects, using `null` for TOP and `[]` for
BOTTOM. The API and CLI expose graph, query and mask-operation statistics.
