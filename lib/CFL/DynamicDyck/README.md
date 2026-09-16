# Dynamic Bidirected Dyck Reachability

`CanaryDynamicDyck` is a port of the algorithms from
Yuanbo Li, Kris Satya, and Qirun Zhang's
[*Efficient Algorithms for Dynamic Bidirected Dyck-Reachability*](https://doi.org/10.1145/3498724)
(POPL 2022), supplied in the
[`dynamic_bidirected_dyck` repository](https://github.com/yuanboli233/dynamic_bidirected_dyck).
The original algorithm bodies are retained, with the adaptations and bug
fixes below. DDlog and unrelated static CFL implementations are outside this
module's scope.

## Organization and source mapping

| Lotus location | Original code / responsibility |
|---|---|
| `include/CFL/DynamicDyck/Solver.h` | Public numeric-vertex library interface |
| `include/CFL/DynamicDyck/IO.h` | Original string-ID file workflow and timing results |
| `include/CFL/DynamicDyck/Statistics.h` | Statistics shared by the numeric and file interfaces |
| `include/CFL/DynamicDyck/Detail/Engine.h` | Per-instance ownership of original global state and engine declarations |
| `include/CFL/DynamicDyck/Detail/DisjointSet.h` | Original fully compressed representative structure |
| `include/CFL/DynamicDyck/Detail/Adjacency.h` | Original `Matrix1` and `CFLHashMap` adjacency implementation |
| `include/CFL/DynamicDyck/Detail/DotParser.h` | Original `SimpleDotParser` declarations |
| `include/CFL/DynamicDyck/Detail/IndexedList.h` | Original indexed linked worklists |
| `lib/CFL/DynamicDyck/Detail/Engine.cpp` | Original weighted merging, insertion, splitting, deletion, preprocessing, and file workflow |
| `lib/CFL/DynamicDyck/Detail/CycleDeletion.cpp` | Cycle-deletion correctness fix |
| `lib/CFL/DynamicDyck/Detail/DotParser.cpp` | Original string-ID DOT parser definitions |
| `tools/cfl/dynamic-dyck` | CLI |
| `benchmarks/real-world/CFL/DynamicDyck` | Original DaCapo/TAL benchmark inputs and an optional subset/verification runner |
| `scripts/cfl/dynamic-dyck` | Original C++ evaluation scripts with repository path adaptation |

Public interfaces remain at the module root; backend headers and their source
files both live in `Detail/`. Header-only adjacency, disjoint-set, and indexed
list structures need no separate implementation file. `IO.h` depends on the
shared statistics rather than the numeric solver interface.

The weighted-graph routines `inc_weight`, `dec_weight`, `mainproc`, `insert`,
`split`, both `split_further` overloads, `weight_split_condition`,
`arrayreach`, and `arrayversion` retain the original algorithm. In particular,
merges choose the representative using quotient node degree, the disjoint
set uses full path compression, and predecessor worklists use the original
indexed linked lists. Algorithm state belongs to one engine instance, so
separate library analyses do not share state. The numeric wrapper can extend
the graph with new vertices; the file workflow preallocates all sequence
endpoints exactly as the original program does.

## Explicit bug fixes

* **Cyclic deletion:** the original splitting restrictions can retain false,
  mutually supporting summaries. The old quotient's opening predecessor
  region is checked before deleting an edge. If it contains a cycle, the
  remaining original graph is preprocessed again by the original algorithm.
  Otherwise the original deletion/splitting routines run. This conservative
  full-graph fallback follows the issue described in
  [Zhang's 2024 correction](https://arxiv.org/html/2401.03570v1).
* **Preprocessing reset:** every preprocessing call starts with fresh
  representatives, weights, weight indexes, worklists, and component sets.
  The original recompute mode reused global representatives across updates.
* **Adjacency bookkeeping:** duplicate insertions and missing deletions do not
  change degree counters; empty neighbor entries are removed. This prevents
  inflated degrees and underflow. Initial weights populate both endpoint
  indexes, and weight-prefix matching includes the separator after the ID.
* **Representative lookup:** iterative full path compression preserves the
  original operation while avoiding recursive lookup stack overflow.
* **Input handling:** arrow matching uses the complete `->` delimiter,
  opening/closing kinds match the prefix rather than text in the type suffix,
  IDs are consistently trimmed, wrappers/blank lines are skipped, and unreadable
  files or invalid update records report errors instead of using invalid IDs.

The original recursive component-splitting procedure is retained for acyclic
deletion. Cyclic deletion can rebuild the whole graph. Concurrent access to
one solver requires synchronization because queries compress paths.

## Library API

```cpp
#include "CFL/DynamicDyck/Solver.h"
using namespace lotus::cfl::dynamic_dyck;
Solver solver;
solver.insertEdge({10, 30, 0});
solver.insertEdge({20, 30, 0});
bool before = solver.connected(10, 20); // true
solver.deleteEdge({30, 20, 0, Parenthesis::Close});
bool after = solver.connected(10, 20);  // false
```

Each `Edge` denotes an opening arc and its complementary closing reverse;
supplying either arc operates on the pair. Duplicate pairs are set-valued,
and absent deletion is a no-op. Distinct labels and oppositely oriented
opening pairs remain distinct. Vertex IDs in the numeric API are signed
64-bit integers. Insertion creates endpoints; deletion does not create them.
Known vertices are reflexively reachable; queries involving unknown vertices
return false. Representatives can change after updates. Solvers are movable
and noncopyable.

`Solver(Graph)` batches initial preprocessing. `graph()` exports canonical
opening pairs; `components()` exports sorted partitions. `parseDot` and
`parseUpdates` provide a stricter numeric convenience interface. To use the
original opaque string IDs and label suffixes, call `runFiles` from `IO.h`.
That workflow returns the original measured elapsed time and final partitions.
Only one Dyck language is supported; bracket families, neutral edges, and
interleaved languages are different problems.

## Original command-line and benchmark workflow

```sh
cmake --build build --target lotus-cfl-dynamic-dyck dynamic_dyck_test
build/bin/lotus-cfl-dynamic-dyck 1 initial.dot updates.seq
build/bin/lotus-cfl-dynamic-dyck 0 initial.dot updates.seq
ctest --test-dir build -R dynamic_dyck --output-on-failure
```

The CLI reads the original `u->v[label="op--TYPE"]` / `cp--TYPE` initial
records and `A|D SOURCE TARGET op--TYPE|cp--TYPE` updates. Node IDs and type
suffixes are interned as strings. Default stdout is the original single
elapsed-seconds value followed by a space, with no newline. Dynamic timing
sums the original per-update timing scopes; recompute timing covers
saturation, excluding graph copying. Recompute processes every input record,
including no-ops. `--stats` and `--print-components` are optional diagnostics.

Run the original tables directly from the repository scripts. They resolve
the Lotus binary and benchmark data relative to their own location, preserving
the original cases, mode ordering, and timing output:

```sh
bash scripts/cfl/dynamic-dyck/gen_table3.sh
bash scripts/cfl/dynamic-dyck/gen_table4.sh
```

`DYCK_REACH_BINARY` and `DYCK_BENCHMARK_ROOT` override the binary and data
paths. No benchmark staging target or copied executable is required.
See the benchmark README for case selection, input validation, and comparing
dynamic/recompute partitions. Use a Release build for performance measurements;
timing values need not reproduce the publication's hardware-dependent results.
