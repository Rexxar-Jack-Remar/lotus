# UseTraceSSA: ordered use histories over existing value flow

`UseTraceSSA` is a non-mutating analysis IR and query library. Its public namespace
is `lotus::usetracessa`; the implementation belongs in `lib/IR/UseTraceSSA`, with
public headers in `include/IR/UseTraceSSA`.

**Integration status.** `CanaryUseTraceSSASVFG` builds UseTraceSSA directly from
this checkout's `lib/IR/SVFG` result and the LLVM module CFG. It handles the
common scalar, MemorySSA, call/return and phi sites; locations it cannot map
precisely are recorded as graph issues. The lower-level mapper API remains
available for clients with specialized SVFG metadata.

`tools/ir/lotus-ir-usetracessa.cpp` builds ICFG and SVFG from an LLVM module,
then prints the UseTraceSSA graph, answers a reachability query, or runs a
resource defect rule. The native builder adds object-history facts for
`malloc`/`calloc`, `free`, and LLVM loads/stores using SVFG object IDs.
`DefectDetector` exposes double-free, use-after-free, taint, Heartbleed-style
and unchecked-use rules to C++ clients. The latter three need application
facts and models; the CLI currently runs the two resource rules.

```cpp
auto history = buildUseTraceSSAFromLotusSVFG(*svfg, *module);
auto report = DefectDetector(history.graph).run(DefectKind::DoubleFree);
if (report.result.status == QueryStatus::Found) {
  // Inspect report.result.nodes/edges and the graph's node labels.
}
```

## 1. Idea and scope

We define a *use trace* as the ordered uses
of a variable along a CFG path and a *use history* as the set of these traces.
Its construction creates a new pseudo-version after each use and merges histories
at joins. It is not the claim that each version has literally one syntactic use.

For a channel `v`, the core constructs:

```
Definition(v)       history = { epsilon }
Psi(v, site, prior) history = { trace . site | trace in history(prior) }
Phi(v, predecessors) history = union of incoming histories
```

A cycle is a finite representation of loop histories.
History phi nodes are distinct from original LLVM/MSSA value phi definitions.
Each original SSA definition is a separate channel. Uses do not overwrite the
original LLVM instruction or change the executable program.

Existing SVFG edges answer “where can this value or memory content flow?” They
do not, by themselves, answer “which earlier uses occurred on the way here?”
UseTraceSSA retains the existing alias and MemorySSA abstraction and adds the
second relation; it does **not** run another pointer analysis.

```
LOTUS SVFG + ICFG/LLVM site layout + existing points-to object IDs
                         |
         buildUseTraceSSAFromLotusSVFG
                         |  <-- reports unmapped locations as issues
               SVFGConstructionInput
                         |
               SVFGHistoryBuilder
                  /              \
       scalar/MSSA histories     typed SVFG transfers
                  \              /
                  TraceFlowGraph + object-lifetime lanes
                            |
          event automata x context-sensitive reachability
                            |
             Found / NotFound / Unknown + witness
```

Sources, sinks, successful checks, external semantics, resource effects and abstraction completeness must be specified by the client/front end.

## 2. Invariants that matter

* A use is atomic: repeated operands of the same channel at one site share one
  after-use version. All reads happen before that site's definitions. Distinct
  semantic phases (call input/output, load mu/result, store input/chi) need
  distinct sites when their order matters.
* CFG edges have identities, not just `(from, to)` endpoints. Phi operands,
  branch assumptions and exceptional definitions can live on virtual edge
  regions. Parallel edges remain distinct. LLVM's CFG is not split or changed.
* Reachable missing/duplicate/non-dominating SSA definitions are rejected.
  Unreachable regions are retained but have no fabricated use histories.
* A local native def-to-consumer edge is **replaced**, not just copied. Its
  source becomes the channel's **after-use port at that consumer**, and its
  destination is the consumer's output. Copying the original def-use shortcut
  alongside this edge would bypass intervening checks. `SVFGAdapter` rejects
  missing or wrong-channel consumption bindings.
* Pointee bytes, pointer addresses and object lifetime state are different
  domains. A tainted buffer address is not a tainted buffer's contents.
* May-points-to overlap is not an equivalence relation. `{A,B}` and `{B,C}` do
  not justify merging A and C. Points-to sets are copied, not union-find merged.
  Unknown/TOP and known-empty/BOTTOM are distinct.
* A singleton may-points-to set is not automatically a must-alias or must-effect
  fact: it may include null behavior, summary objects or conditional effects.
  A `May` event retains a no-effect transition; it cannot serve as a hard trap.
* Checking a pointer or length is not enough to establish a safety fact. Attach
  `NonNull` or `Sanitize` to the appropriate **successful CFG edge/validated
  use port**, not to the comparison instruction on both outcomes.
* Branch guard strings are provenance. They are **not** solved as formulas.
  Feasibility, bounds relationships, path correlations and concurrency require
  additional analyses.

## 3. Components

| Header | Responsibility |
| --- | --- |
| `UseTraceSSA.h` | SSA validation, edge-expanded CFG, pruned history phi insertion, renaming, use/definition lookup and history witnesses |
| `TraceFlowGraph.h` | Layered history and typed transfer graph, events, certainty, object guards, provenance, DOT/JSON output |
| `SVFGHistoryBuilder.h` | Constructs channel histories from located, normalized native SVFG records |
| `SVFGAdapter.h` | Imports already-bound native records without def-use shortcuts; validates before mutation |
| `LotusSVFG.h` | Native SVFG construction entry point, optional mapper wrapper and edge metadata copier |
| `ResourceHistory.h` | Per-abstract-object chronological state, alias-preserving release/allocation histories, matched call splicing |
| `Query.h` | Traps, event automata, exact call/return matching in the supplied graph, witnesses, coverage and batch queries |
| `Models.h` | Explicit library summaries and taint, double-free, use-after-free, unchecked-use and Heartbleed-style query factories |
| `ReachabilityIndex.h` | SCC-based structural reachability cache, revision checks |
| `LLVMHistory.h` | Optional non-mutating LLVM SSA/site importer, edge-local null guards |
| `LLVMFlow.h` | Optional scalar transfer supplementation; deliberately no invented load/store memory flow |
| `HistoryPass.h` | Optional LLVM new-PM analysis/printer and legacy function pass |
| `DefectDetector.h` | Defect-query interface with typed outcomes and witness provenance |

## Command-line entry point

```sh
cmake --build build --target lotus-ir-usetracessa
build/bin/lotus-ir-usetracessa input.bc --format=json
build/bin/lotus-ir-usetracessa input.bc --source-node=12 --sink-node=34
build/bin/lotus-ir-usetracessa input.bc --check=double-free
build/bin/lotus-ir-usetracessa input.bc --check=use-after-free
```

The tool calls Lotus `ICFGBuilder` and `SVFGBuilder` with pointer analysis and
MemorySSA enabled before constructing UseTraceSSA. `--format=dot` emits the
overlay graph; `--dump-svfg=path.dot` also saves the source SVFG. A defect
`Found` result is a **potential** bug witness in the supplied abstraction;
it is not proof that the path is feasible. Missing models, unresolved aliases,
and interprocedural resource effects are recorded as issues; a negative query
with such gaps is `Unknown`. The checker does not yet infer taint policies,
sanitizers, full external-call semantics or cross-function resource histories
the way a dedicated checker such as Saber can.
