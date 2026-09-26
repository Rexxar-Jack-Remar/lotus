# Lotus Unit Tests

`tests/unit` contains subsystem-level gtests for Lotus. Its structure follows
the production taxonomy so contributors can move between `include/`, `lib/`,
and `tests/unit/` without translating historical names.

## Layout

Top-level test buckets:

- `Alias/` for alias-analysis and pointer-analysis tests
- `Analysis/` for general analyses, with subdirectories such as `CFG/`,
  `DebugInfo/`, `Loop/`, `NullPointer/`, `Profile/`, and `Purity/`
- `CFL/`
- `Checker/`
- `Concurrency/` with subdirectories such as `MHP/`, `OpenMP/`, `MPI/`,
  `CUDA/`, `LinuxKernel/`, and `Utils/`
- `Dataflow/`
- `Fuzzing/`
- `IR/`
- `Solvers/`
- `SymbolicExecution/`
- `TypeHierarchy/`
- `Utils/`
- `Verification/`
- `TestUtils/` for shared test-only headers

When adding tests, mirror the source tree where practical:

- `include/Alias/...` and `lib/Alias/...` -> `tests/unit/Alias/...`
- `include/Dataflow/...` and `lib/Dataflow/...` -> `tests/unit/Dataflow/...`
- `include/Analysis/CFG/...` and `lib/Analysis/CFG/...` -> `tests/unit/Analysis/CFG/...`

## Build And Run

```bash
cmake -S . -B build
cmake --build build --target <test_target>
ctest --test-dir build --output-on-failure
```

Examples:

- build one target: `cmake --build build --target analysis_tests`
- run one case: `ctest --test-dir build -R CFGUtilitiesTest --output-on-failure`

Every gtest case is discovered individually. Tests also carry a cost/dependency
label:

- `unit` for fast, hermetic tests
- `component` for multi-component or real-concurrency harnesses
- `integration` for tests that invoke external tools or generated fixtures

Run a layer with `ctest --test-dir build -L unit`, `-L component`, or
`-L integration`. New suites default to `unit`; pass `TEST_KIND COMPONENT` or
`TEST_KIND INTEGRATION` to `add_lotus_test_suite` when appropriate.

CTest case granularity does not require one executable per source file. Keep
related test sources in one suite so LLVM and Lotus libraries are linked only
once. A child directory can declare sources with
`lotus_collect_test_sources(<suite> ...)`; the parent then creates the single
binary with `add_lotus_collected_test_suite(<suite> ...)`. GTest discovery
still registers every case independently.

Each subsystem therefore builds one merged gtest binary: `alias_tests`,
`analysis_tests`, `cfl_tests`, `concurrency_tests`, `dataflow_tests`,
`ir_tests`, `solvers_tests`, `utils_tests`, `verification_tests`, plus the
already merged `checker_tests`. Integration suites that need generated fixtures
(`loop_analysis_tests`, `type_hierarchy_tests`) and standalone harness binaries
with their own `main` remain separate. `concurrency_core_tests` and
`solver_tests` are kept as aggregate build entry points. When adding tests,
prefer collecting them into the matching subsystem binary instead of creating a
new executable.

## Adding Tests

Use the small set of helpers in `tests/unit/UnitTestHelpers.cmake`:

- `add_lotus_test_suite` for normal multi-source suites
- `add_lotus_targeted_test` for a single-source test with explicit libraries
- `add_lotus_concurrency_test_suite` for concurrency suites sharing the common
  link set
- `lotus_collect_test_sources` and `add_lotus_collected_test_suite` when child
  directories contribute to one linked binary

Keep test target names stable unless you intentionally want to update external
scripts or CI filters.
