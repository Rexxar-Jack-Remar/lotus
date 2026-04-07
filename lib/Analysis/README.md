# Analysis Library

Core analyses built on LLVM IR.

| Subdir | Purpose |
|--------|---------|
| **CFG** | Reachability, dominators, post-dominators, topological order, back edges, code metrics. |
| **Crypto** | CT-LLVM: constant-time side-channel analysis (ECOP 24 CtChecker-related). |
| **DebugInfo** | MetadataManager, LoopStructure, debug-info-driven annotations. |
| **Loop** | Loop forest/structure, dependence graphs, SCC DAGs, invariants, induction variables, loop-carried dependences, iteration-space and memory-cloning analyses. See `Loop/README.md`. |
| **NullPointer** | Null-check, null-flow, null-equivalence; context-sensitive variants. |
| **SymbolicExecution** | Symbolic execution state, constraints, taint modeling, and analysis driver infrastructure. See `SymbolicExecution/README.md`. |
| **TypeHierarchy** | C++ class hierarchy, vtable reconstruction, virtual-call resolution. |
