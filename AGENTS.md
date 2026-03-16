# AGENTS.md — Lotus Program Analysis Framework

## Project Overview

Lotus is a **program analysis, verification, and optimization framework** built on LLVM. It provides alias analysis, intermediate representations, dataflow analysis, constraint solvers, abstract interpretation, and bug checkers.

- **Language**: C++14
- **Dependencies**: LLVM 14.x, Z3, CMake 3.10+
- **Docs**: https://zju-pl.github.io/lotus

## Repository Layout

```
lotus/
├── include/           # Public headers (mirrors lib structure)
│   ├── Alias/         # Alias analysis (DyckAA, AserPTA, LotusAA, SparrowAA, etc.)
│   ├── Analysis/      # Analysis utilities (NullPointer, Concurrency, CFG, etc.)
│   ├── CFL/           # CFL reachability
│   ├── Checker/       # Bug checkers (Concurrency, FiTx, GVFA, KINT, Pulse)
│   ├── Dataflow/      # APA, IFDS/IDE, Mono, NPA, WPDS
│   ├── IR/            # GSA, ICFG, MemorySSA, PDG, SSI, SVFG, vSSA
│   ├── Solvers/       # SMT
│   ├── Transform/     # LLVM bitcode transformations
│   ├── Utils/         # LLVM utilities, ThreadPool, formats, etc.
│   └── Verification/  # SIFA, CLAM, SymbolicAbstraction, Seahorn
├── lib/               # Implementations (mirrors include)
├── tools/             # Command-line tools (alias, checker, verifier, ir, etc.)
├── tests/             # GTest-based tests (tests/unit/ mirrors subsystems)
├── benchmarks/        # Benchmark programs
├── third-party/       # CUDD, WPDS, spdlog
├── scripts/           # Python and build utilities
└── docs/              # Sphinx documentation (source/)
```

**Convention**: `include/` holds headers; `lib/` holds `.cpp` sources. Directory names match between them (e.g., `include/Alias/DyckAA/`, `lib/Alias/DyckAA/`).

## Build System

- **CMake**: Root `CMakeLists.txt` configures LLVM, Z3, optional Boost/CLAM/SeaHorn
- **Libraries**: Static libs prefixed `Canary*` (e.g., `CanaryDyckAA`, `CanaryPDG`) — legacy naming
- **Tools**: Binaries go to `build/bin/` (e.g., `lotus-gvfa`, `lotus-aa`, `lotus-kint`, `clam`)
- **Tests**: Use `add_lotus_test()` / `add_lotus_pdg_test()`; run from `build/` with `ctest`

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug   # Debug for assertions
make -j$(nproc)
make test
```

Custom LLVM path: `cmake .. -DLLVM_BUILD_PATH=/path/to/llvm/lib/cmake/llvm`

## Coding Conventions

| Element        | Style       | Example                    |
|----------------|------------|----------------------------|
| Classes        | CamelCase  | `NullCheckAnalysis`        |
| Functions      | camelCase  | `getPointsToSet`           |
| Variables      | snake_case | `points_to_set`            |
| Constants      | UPPER_CASE | `MAX_ITERATIONS`           |
| Member vars    | m_ or _    | `m_AnalysisMap`            |
| Indentation    | 2 spaces   | —                          |
| Line length    | ≤ 100 chars| —                          |
| Headers        | `#pragma once` or include guards | — |

**Namespaces**: Use `lotus::` for framework code (e.g., `lotus::sifa`). Some modules use `using namespace llvm` in `.cpp` files.

## Architecture

```
Tools (aser-aa, dyck-aa, lotus-gvfa, lotus-kint, clam, etc.)
    ↓
Analysis Applications (Checkers, Optimization, Verification, etc.)
    ↓
Core: Alias Analysis | IR  | Dataflow  Analysis | Abstract Interpretation
    ↓
LLVM (Module, Function, BasicBlock, Instruction) | Solvers
```

## Adding New Code

### New Alias Analysis

1. Create `include/Alias/MyAA/` and `lib/Alias/MyAA/`
2. Implement `ModulePass` (or `FunctionPass`) with `runOnModule`/`runOnFunction`
3. Optionally inherit `llvm::AliasAnalysis` and implement `alias()`, `getPointsToSet`
4. Add `CMakeLists.txt` under `lib/Alias/MyAA/` and `add_subdirectory(MyAA)` in `lib/Alias/CMakeLists.txt`
5. Add tool in `tools/alias/` if needed

### New Bug Checker

1. Add `include/Checker/MyChecker/` and `lib/Checker/MyChecker/`
2. Extend `llvm::ModulePass` (or a checker base); use `BugReportMgr` for reporting
3. Add to `tools/checker/` and wire into existing tools (e.g., `lotus-gvfa`)

### New Pass / Analysis

1. Declare dependencies in `getAnalysisUsage()` (e.g., `AU.addRequired<DyckAliasAnalysis>()`)
2. Use `RegisterPass<T>` for legacy pass registration
3. Add CMake target and link against required `Canary*` libs


## Testing

- Tests live under `tests/unit/` (Pointer, IR, Verification, DataFlow, etc.)
- Use GTest: `add_lotus_test(name source.cpp)` or `add_lotus_pdg_test` for PDG-heavy tests
- Tests link `lotus_test_utils` (includes common libs)
- Run: `cd build && ctest --output-on-failure`


## Documentation

- User guide, architecture, major components: `docs/source/user_guide/`
- Developer guide (adding analyses, checkers, domains): `docs/source/developer/developer_guide.rst`
- Component READMEs: e.g., `lib/Alias/LotusAA/README.md`, `lib/CFL/CSIndex/README.md`
