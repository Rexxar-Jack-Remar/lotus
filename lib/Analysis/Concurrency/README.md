# Analysis of Concurrent Programs

## Utilities

- **ThreadAPI**: Provides an API for identifying and categorizing thread-related operations (fork, join, lock, unlock, etc.) in LLVM IR.
- **ThreadFlowGraph**: Implements a graph representation of thread synchronization operations and their ordering relationships.
- **FBVClock**: Implements a fast bit-vector clock system for tracking happens-before relationships in concurrent programs.
- **BVClock**: Provides a bit-vector clock implementation for vector clocks used in concurrency analysis.

## Analyses 
- **LockSetAnalysis**: Performs may/must lock set analysis to track which locks are held at each program point.
- **HappensBeforeAnalysis**: Determines happens-before relationships between instructions using MHP analysis.
- **MHPAnalysis**: Implements may-happen-in-parallel analysis to determine which instructions can execute concurrently.
- **StaticVectorClockMHP**: Implements a static vector clock-based approach for MHP analysis.
- **StaticThreadSharingAnalysis**: Analyzes which memory locations are shared between threads using static analysis.
- **MemUseDefAnalysis**: Performs memory use-def analysis based on MemorySSA to track memory dependencies.
- **EscapeAnalysis**: Determines which values escape their thread-local scope and become shared between threads.
- **JoinTargetAnalysis**: For each pthread_join, computes the set of pthread_create calls that may be joined (join's arg0 may alias fork's arg0). Supports unambiguous-join reasoning for MHP refinement.


## Data-race checker (Ultimate borrows)

- **Sync object exclusion**: Accesses to lock/cond/barrier objects (first arg of pthread_mutex_*, pthread_cond_*, pthread_barrier_*) are excluded from race checking.
- **Data-race report schema**: ConcurrencyBugReport supports optional **DataRaceInfo** (access path, read/write, conflicting pair) for witness/SARIF.


## Limitations

- Missing: Support for std::memory_order_relaxed, acquire, release, etc. The ThreadAPI maps atoms roughly to locks/barriers but doesn't handle the fine-grained "synchronizes-with" relationships of weak atomics
