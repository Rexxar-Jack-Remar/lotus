# Partial Evaluation (PE) – LLPE for Lotus

This directory contains the **LLPE** (LLVM Partial Evaluator) engine, upgraded from LLVM 5.0 to **LLVM 14.x** and integrated into Lotus.

The driver is at `Integrator.cpp`.

## Components

- **Headers**: `include/Optimization/PartialEvaluation/` – LLPE.h, ShadowInlines.h, SharedTree.h, LLPECopyPaste.h
- **Library**: Static library `CanaryPE` built from sources in `lib/Optimization/PartialEvaluation/`
- **Passes**: Legacy `ModulePass`es:
  - `llpe-analysis` – LLPE analysis (specialisation context, hypothetical constant folding)
  - `llpe` – LLPE integrator (commits specialisation)



## Using the PE passes

Link your tool with `CanaryPE` (and any Lotus libraries it depends on). Register and run the legacy passes as usual:

- Run the analysis: `LLPEAnalysisPass` (ID `llpe-analysis`).
- Run the integrator: `LLPEPass` (ID `llpe`) after the analysis; it calls `commit()` on the analysis result.

Example (conceptual): add `llpe-analysis` and `llpe` to your legacy pass pipeline; the root function can be set with the `-llpe-root=<name>` option (default: `main`).

## Optional integration

The PE library is built via `add_subdirectory(PartialEvaluation)` in `lib/Optimization/CMakeLists.txt`. It is separate from the scalar/IPO/pipeline optimization libraries; tools that need partial evaluation should link `CanaryPE` explicitly.


## References

I/O Optimisation and elimination via partial evaluation.
Christopher S.F. Smowton
