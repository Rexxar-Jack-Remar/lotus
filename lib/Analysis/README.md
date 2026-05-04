# Analysis Library

Core analyses built on LLVM IR.

| Subdir | Purpose |
|--------|---------|
| **CFG** | Reachability, dominators, post-dominators, topological order, back edges, code metrics. |
| **DebugInfo** | MetadataManager, LoopStructure, debug-info-driven annotations. |
| **FeatureExtraction** | Memory-related feature extraction using Sea-DSA for ML-oriented analysis workloads. |
| **Loop** | Loop forest/structure, dependence graphs, SCC DAGs, invariants, induction variables, loop-carried dependences, iteration-space and memory-cloning analyses. See `Loop/README.md`. |
| **NullPointer** | Null-check, null-flow, null-equivalence; context-sensitive variants. |
| **SymbolicExecution** | Symbolic execution state, constraints, taint modeling, and analysis driver infrastructure. See `SymbolicExecution/README.md`. |
| **TypeHierarchy** | C++ class hierarchy, vtable reconstruction, virtual-call resolution. |

Security-oriented side-channel analyses and transformations now live under
`lib/Security`.
