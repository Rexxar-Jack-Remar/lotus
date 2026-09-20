# GPG points-to analysis

This directory contains the LLVM-native migration of the generalized
points-to graph analysis described by Gharat, Khedker, and Mycroft,
*ACM TOPLAS* 42(2), 2020. The reference GCC 4.7.2 plugin is the
`GPG-based-Points-to-Analysis` implementation by the paper's authors.

The default mode is exhaustive, flow-sensitive, field-sensitive, and fully
context-sensitive. `FICS` and `FICI` modes are also available for comparison
with the variants exposed by the reference implementation.

## Architecture

| Component | Responsibility |
| --- | --- |
| `IndirectionList` | Field-sensitive indirection lists, wildcard fields, k-limiting, and summarized remainders from Appendix B. |
| `GPU` | Generalized points-to updates, TS/SS composition, data-dependence tests, GPU reduction, and queued producers. |
| `Graph` | GPBs and GPG flow edges, boundary definitions, both reaching-GPU analyses, blocking, strong/weak updates, dead-GPU elimination, and coherent coalescing. |
| `ProgramModel` | Stable abstract locations for LLVM SSA values, globals, stack objects, allocation sites, formals, functions, returns, null, and unknown memory. |
| `LLVMFrontend` | Translation of LLVM CFGs and pointer operations into initial GPGs, including GEP fields, aggregate initializers, memory intrinsics, calls, and pointer uses. |
| `GPGAnalysisEngine` | Bottom-up call inlining, SCC refinement for recursion, delayed function-pointer resolution, sensitivity variants, and optimization scheduling. |
| `GPGResult` | Statement-specific points-to facts, indirect-call targets, mod/ref summaries, and statistics. |

The migration replaces GCC-specific `tree`, GIMPLE, `basic_block`,
`cgraph_node`, constraint-vector, and plugin-pass APIs with LLVM 14 `Value`,
`Instruction`, `BasicBlock`, `Function`, `Module`, and `CallBase` APIs. SSA
definitions are marked flow-insensitive so barriers cannot hide immutable
def-use information, matching the reference implementation's transitive SSA
resolution.

## Public entry points

- `lotus::gpg::GPGAnalysisEngine` is the direct C++ API.
- `lotus::gpg::GPGAnalysisPass` is the legacy LLVM `ModulePass`.
- `AAConfig::GPG()` selects GPG through `AliasAnalysisWrapper`.
- `lotus-alias-gpg` is the standalone driver.
- `lotus-alias-call-graph --cg-type=gpg` emits a GPG-resolved call graph.

Example:

```bash
build/bin/lotus-alias-gpg --mode=fscs --print-pts \
  --print-call-graph --print-modref input.bc
```

Useful comparison modes are `--mode=fics` and `--mode=fici`. The default heap
indirection-list limit is the paper's `k = 3`; change it with `--heap-k`.

## Algorithm-to-source map

- Paper Sections 3-4 and Appendix A-B: `IndirectionList.cpp`, `GPU.cpp`, and
  the reaching analyses in `Graph.cpp`.
- Paper Sections 5-6: dead-GPU elimination, empty-GPB elimination, and
  coherent partition construction in `Graph.cpp`.
- Paper Section 7 and Appendix C: call expansion, recursive SCC refinement,
  and delayed indirect calls in `Graph.cpp` and `Analysis.cpp`.
- Paper Section 8: query provenance on GPUs and back-annotation in
  `Result.cpp`.
- GCC parser and initial-GPG construction: replaced by `ProgramModel.cpp` and
  `LLVMFrontend.cpp`.

Unit coverage lives in `tests/unit/Alias/InclusionBased/GPG/`.
