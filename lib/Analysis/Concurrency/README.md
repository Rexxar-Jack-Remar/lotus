# Analysis of Concurrent Programs

## Directory layout

Headers live under `include/Analysis/Concurrency/`, sources under `lib/Analysis/Concurrency/`, grouped by subdirectory:

| Subdirectory | Contents |
|--------------|----------|
| **Utils/** | ThreadAPI, ThreadFlowGraph, ThreadInfo, BVClock, FBVClock, LanguageModel/ (Cpp11, OpenMP) |
| **MHP/** | MHPAnalysis, StaticVectorClockMHP, HappensBeforeAnalysis, Cpp11Atomics |
| **LockSet/** | LockSetAnalysis |
| **Memory/** | EscapeAnalysis, StaticThreadSharingAnalysis, MemUseDefAnalysis |
| **JoinTarget/** | JoinTargetAnalysis |

Include paths use these subdirs, e.g. `Analysis/Concurrency/Utils/ThreadAPI.h`, `Analysis/Concurrency/MHP/MHPAnalysis.h`.

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


## Modern C++ Support (C++11/14/17/20)

### Threading Primitives
- **std::thread**: Full support for thread creation, joining, and detaching
- **std::jthread** (C++20): Automatic joining thread support

### Mutex and Locking
- **std::mutex**: lock, try_lock, unlock
- **std::recursive_mutex**: Recursive mutex operations
- **std::shared_mutex** (C++17): Shared/exclusive locking (lock_shared, lock, unlock_shared, unlock)
- **std::shared_timed_mutex** (C++17): Timed shared locking

### RAII Lock Wrappers
- **std::lock_guard**: RAII exclusive lock wrapper
- **std::unique_lock**: Movable RAII lock with manual lock/unlock support
- **std::scoped_lock** (C++17): Multi-lock RAII wrapper for deadlock-free locking
- **std::shared_lock** (C++17): RAII shared lock wrapper

### Synchronization Primitives
- **std::condition_variable**: wait, notify_one, notify_all
- **std::call_once** / **std::once_flag**: One-time initialization
- **std::future** / **std::promise**: Task-based synchronization (get, wait, set_value, set_exception)
- **std::async**: Asynchronous task creation
- **std::latch** (C++20): Single-use countdown synchronization
- **std::barrier** (C++20): Reusable thread barrier
- **std::counting_semaphore** / **std::binary_semaphore** (C++20): Semaphore primitives

### Atomic Operations
- **Full memory ordering support**: relaxed, acquire, release, acq_rel, seq_cst
- **Atomic operations**: load, store, exchange, compare_exchange, fetch_add, etc.
- **Fence instructions**: atomic_thread_fence with all memory orders
- **Synchronizes-with relationship tracking**: Proper acquire-release semantics for happens-before analysis
- **Release sequences**: RMW chains properly tracked

## OpenMP Support (3.0 - 5.x)

### Basic Constructs
- **#pragma omp parallel**: Parallel regions (__kmpc_fork_call)
- **#pragma omp barrier**: Explicit barriers
- **#pragma omp critical**: Critical sections
- **#pragma omp master**: Master-only regions
- **#pragma omp single**: Single-thread execution
- **#pragma omp ordered**: Ordered execution

### Worksharing
- **#pragma omp for**: Parallel loops (static, dynamic, guided scheduling)
- **#pragma omp sections**: Independent code sections
- **#pragma omp single**: Single execution with implicit barrier

### Task-Based Parallelism (3.0+)
- **#pragma omp task**: Explicit task creation (__kmpc_omp_task)
- **#pragma omp taskwait**: Wait for child tasks (__kmpc_omp_taskwait)
- **#pragma omp taskyield**: Yield to other tasks (__kmpc_omp_taskyield)
- **#pragma omp taskgroup**: Task group synchronization
- **#pragma omp taskloop** (4.5+): Loop-based task creation
- **Task dependencies** (4.0+): depend clauses for task ordering

### Advanced Features
- **Target offloading** (4.0+): Device execution support
- **Atomic operations**: __kmpc_atomic_start/end
- **Flush**: Memory fence operations
- **Cancellation** (4.0+): Cancellation points and cancel constructs
- **OpenMP locks**: omp_set_lock, omp_unset_lock, nested locks

## Analysis Capabilities

### Lock Set Analysis
- **May-lockset**: Over-approximation of locks that might be held
- **Must-lockset**: Under-approximation of locks that must be held
- **Read/write lock tracking**: Separate tracking for shared_mutex read vs write locks
- **RAII lock tracking**: Automatic tracking of lock_guard, unique_lock, scoped_lock lifetimes
- **Semaphore tracking**: C++20 counting_semaphore and binary_semaphore
- **Lock ordering**: Deadlock detection via lock order inversion analysis
- **Interprocedural**: Function summaries for context-sensitive lock analysis

### Happens-Before Analysis
- **Atomic synchronization**: Full acquire-release semantics with all memory orders
- **Fence synchronization**: atomic_thread_fence properly modeled
- **Future/Promise synchronization**: promise.set_value() → future.get() edges
- **call_once synchronization**: First execution synchronizes-with subsequent calls
- **Latch synchronization**: count_down → wait edges
- **Barrier synchronization**: arrive → wait edges at barriers
- **Release sequences**: RMW chains properly modeled for happens-before

### Data Race Detection
- **Atomic awareness**: Uses enhanced memory ordering semantics
- **RAII lock protection**: Understands implicit acquire/release from RAII locks
- **Shared vs exclusive locks**: Distinguishes reader-writer lock semantics

### Lock Mismatch Detection
- **RAII lock patterns**: Detects mismatched lock_guard, unique_lock usage
- **unique_lock manual operations**: Tracks manual lock()/unlock() calls
- **Shared lock patterns**: Validates shared_lock and shared_mutex usage

## Limitations

- **RAII destructor tracking**: Currently uses conservative approximation for destructor lock releases. Full lifetime analysis via RAIILockTracker available but not yet fully integrated.
- **std::memory_order_consume**: Recognized but treated as acquire (per C++ standard recommendation)
- **Weak atomics precision**: Relaxed atomics tracked but no value-flow analysis
- **Task dependencies**: OpenMP task depend clauses recognized but dependency graph not yet built
- **Target offloading**: OpenMP target constructs recognized but no device-specific analysis
