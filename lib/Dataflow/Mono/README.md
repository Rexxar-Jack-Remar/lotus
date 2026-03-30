# Monotone Dataflow Analysis

Monotone dataflow analysis frameworks for intraprocedural and interprocedural analysis.

**What gets compiled:** Only the analysis implementations in this directory.
Framework code under `include/Dataflow/Mono/{Core,Solver,Container,Support}`
is header-only and compiled into consumers.

**Layout (aligned with include/Dataflow/Mono/):**

- **Core/** — `Problem.h`, `Domain.h`, `CallStringContext.h`,
  `CallStringSolver.h`
- **Solver/** — `IntraSolver.h`, `InterSolver.h`
- **Container/** — `BitVectorSet.h`, `Traits.h`
- **Support/** — `Result.h`, `MonoDebug.h`, `Soundness.h`
- **Analyses/Intra/** — Implementation files `Intra*.cpp` (headers `Intra*.h`)
- **Analyses/Inter/** — Implementation files `Inter*.cpp` (headers `Inter*.h`)

## Quick include and migration guide

- Modern headers:
  - `#include "Dataflow/Mono/Core/Problem.h"`
  - `#include "Dataflow/Mono/Solver/IntraSolver.h"`
- Legacy includes that were split up:
  - `Dataflow/Mono/DataFlow.h`
  - `Dataflow/Mono/Solver/IntraMonoSolver.h`

`Core/IntraMonoSolver.h` remains only as a compatibility shim re-exporting the
authoritative implementation in `Solver/IntraSolver.h`.

## Architecture

- `IntraMonoProblem` models intraprocedural analyses with `normalFlow()`,
  `merge()`, `equal_to()`, and `initialSeeds()`.
- `InterMonoProblem` extends that interface with `callFlow()`,
  `returnFlow()`, and `callToRetFlow()`.
- `CallStringContext` and `CallStringSolver` provide bounded call-string
  context sensitivity for interprocedural clients.
- `Container/BitVectorSet.h` provides a bit-vector-backed set implementation
  for larger finite universes.

## API notes

- `CallStringInterProceduralDataFlowEngine::applyForwardFromSeeds()` returns
  `std::unique_ptr<ResultTy>`.
- Callee and return-site resolution in the call-string engine is driven by the
  provided ICFG (`getCalleesOfCallAt`, `getReturnSitesOfCallAt`, etc.), not a
  separate callback parameter.
