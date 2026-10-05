# GSAF layout

Public headers mirror this directory under `include/Checker/GSAF`.

| Directory | Responsibility |
| --- | --- |
| `API` | Vulnerability and trace definitions, registration, composition, and the query view over Lotus annotation models. `DefaultModels.h.in` embeds the profiles from `config/gsaf`. |
| `Engine` | Module/function analysis, taint traversal, SMT encoding, summaries, analysis traces, composite trace matching and graph statistics. |
| `Checkers` | Built-in vulnerability policies: use after free and taint. |
| `Report` | Diagnostic builders/decorators and trace scoring. Rendered reports use the shared checker framework. |
| `Support` | Engine options, native GVFG query helpers, checker analysis access, mask containers and graph-object ordering. |

`API/Trace` contains the analysis trace and its scope-aware builder. It is
shared by the engine and reporting. `Support/MaskMap` and
`Support/ObjectOrder` expose specific helpers rather than a catch-all utility
header. The engine contains four header/source pairs: `Checker`,
`FunctionAnalyzer`, `Solver`, and `Summaries`. Small graph-statistics and composite-orchestration
helpers share the module-pass implementation; symbolic and trace summaries
share one summary header.

Reusable facilities belong to their Lotus subsystems: library facts in
`Annotation`, heap analysis in `Analysis/Memory`, debug expression rendering in `Analysis/DebugInfo`,
diagnostic templates/composition in `Checker/Framework/DiagnosticEvent` and
`CheckerDiagnostic`, and graph/solver APIs
in `IR/GVFG`. Shared trace containers live in `Utils/Trace`; graph-specific
trace conversion stays in GVFG. LLVM/GVFG narrative rendering stays in
`GSAF/Report`; final steps and metadata use the shared `BugReport` implementation.
GSAF does not maintain a second graph representation.

Build and run:

```sh
cmake --build build --target lotus-check checker_tests -j8
build/bin/tests/checker_tests --gtest_filter='GSAF*'
build/bin/lotus-check --engine=gsaf --checks=taint,use-after-free input.bc
```

Use `--checks=all` for the two current built-in checks. Additional checker
implementations should register explicitly through the API and be included
in `initializeBuiltinVulnerabilities()`, so static-library linking retains
those registrations. Update the unified frontend's check descriptors when
adding a built-in check.
