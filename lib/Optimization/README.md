# Optimization Library

Optimization passes and optimization-oriented analyses built on Lotus IR and
memory modeling.

| Subdir | Purpose |
|--------|---------|
| **IP** | Interprocedural memory optimizations built on ShadowMem/MemorySSA, including dead store elimination, redundant load elimination, store sinking, and store-to-load forwarding. |
| **PE** | LLPE-based partial evaluation and specialization for LLVM IR, including inlining, loop peeling, load forwarding, and dead code elimination. |
| **SWPrefetching** | Software prefetching passes for indirect memory accesses and loop-based prefetch insertion experiments. |

## SWPrefetching

The `SWPrefetching/` subdirectory contains the software prefetching
implementation used for indirect-memory-access workloads.

**Benchmarks**

- https://github.com/masabahmad/CRONO

**Related work**

- CGO 2017: Software Prefetching for Indirect Memory Accesses. Sam Ainsworth, Timothy M. Jones. [pdf](https://www.cl.cam.ac.uk/~sa614/papers/Software-Prefetching-CGO2017.pdf) [repo](https://github.com/SamAinsworth/reproduce-cgo2017-paper)
- TOCS 2019: Software Prefetching for Indirect Memory Accesses: A Microarchitectural Perspective. https://github.com/SamAinsworth/reproduce-tocs2019-paper
- EuroSys 2022: PT-GET: profile-guided timely software prefetching. [repo](https://github.com/SabaJamilan/Profile-Guided-Software-Prefetching)
- LLVM loop-data prefetch pass: https://github.com/llvm/llvm-project/blob/main/llvm/lib/Transforms/Scalar/LoopDataPrefetch.cpp
- LLVM loop-data prefetch tests: https://github.com/llvm/llvm-project/tree/main/llvm/test/Transforms/LoopDataPrefetch

## Notes

- Some LICM-related code was taken from LLVM 14 and is used to evaluate alias
  analyses inside Lotus.
