# Optimizations


## Pefetch

**Benchmarks**

- https://github.com/masabahmad/CRONO


Related work

-  CGO 17: Software Prefetching for Indirect Memory Accesses. Sam Ainsworth, Timothy M. Jones. [pdf](https://www.cl.cam.ac.uk/~sa614/papers/Software-Prefetching-CGO2017.pdf) [repo](https://github.com/SamAinsworth/reproduce-cgo2017-paper)
- TOCS 19: Software Prefetching for Indirect Memory Accesses: A Microarchitectural Perspective.https://github.com/SamAinsworth/reproduce-tocs2019-paper
- EuroSys'22: PT-GET: profile-guided timely software prefetching. [repo](https://github.com/SabaJamilan/Profile-Guided-Software-Prefetching)

- LLVM's internal prefetch pass.
https://github.com/llvm/llvm-project/blob/main/llvm/lib/Transforms/Scalar/LoopDataPrefetch.cpp
https://github.com/llvm/llvm-project/tree/main/llvm/test/Transforms/LoopDataPrefetch

## LICM

Taken from LLVM 14.
Used to evaluate alias analsyes inside Lotus.

## Inter-procedural (IP)

The **IP** subdirectory holds inter-procedural memory optimizations built on ShadowMem/MemorySSA: dead store elimination, redundant load elimination, store sinking, and store-to-load forwarding. See [IP/README.md](IP/README.md).

## Partial Evaluation (PE)

The **PE** subdirectory contains LLPE (LLVM Partial Evaluator), upgraded from LLVM 5.0 to LLVM 14.x and integrated into Lotus. It provides specialisation of LLVM IR (function inlining, loop peeling, load forwarding, dead code elimination). See [PE/README.md](PE/README.md) for details and usage.