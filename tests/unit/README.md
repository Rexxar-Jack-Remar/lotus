# Lotus Unit Tests

`tests/unit` contains subsystem-level gtests for Lotus. Its structure follows
the production taxonomy so contributors can move between `include/`, `lib/`,
and `tests/unit/` without translating historical names.

## Layout

Top-level test buckets:

- `Alias/` for alias-analysis and pointer-analysis tests
- `Analysis/` for general analyses, with subdirectories such as `CFG/`,
  `DebugInfo/`, `Loop/`, `NullPointer/`, `Profile/`, `Purity/`, and
  `SymbolicExecution/`
- `CFL/`
- `Checker/`
- `Concurrency/` with subdirectories such as `Threads/`, `OpenMP/`, `MPI/`,
  `CUDA/`, and `Kernel/`
- `Dataflow/`
- `Fuzzing/`
- `IR/`
- `Solvers/`
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

- build one target: `cmake --build build --target icfg_test`
- run one registered test: `ctest --test-dir build -R icfg_test --output-on-failure`

## Adding Tests

Use the subsystem helpers from `tests/unit/UnitTestHelpers.cmake`. Prefer the
most specific helper available, such as:

- `add_lotus_analysis_test`
- `add_lotus_concurrency_test`
- `add_lotus_ir_test`
- `add_lotus_pointer_test`
- `add_lotus_verification_test`
- `add_lotus_targeted_test` only when no subsystem helper fits

Keep test target names stable unless you intentionally want to update external
scripts or CI filters.
