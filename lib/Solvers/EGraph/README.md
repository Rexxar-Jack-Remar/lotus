# Lotus EGraph

`LotusEGraph` is a solver-agnostic C++17 e-graph library for Lotus.

It is modeled after the Rust `egg` library and provides the core surfaces
needed for equality saturation and rewrite-driven normalization:

- `Id`
- `UnionFind`
- `SymbolLang`
- `RecExpr`
- `EGraph`
- `Pattern`
- `Rewrite`
- `Runner`
- `Extractor`
- `MultiPattern`
- lightweight explanation and dot-export surfaces
- no LP extractor surface: the previous Z3-backed DAG extractor was not a
  faithful migration of `egg`'s `good_lp`-based implementation and has been
  removed pending a proper redesign

## Design goals

- reusable outside SMT-specific clients
- small, header-driven C++ surface compatible with Lotus build conventions
- preserve `egg` semantics for the core e-graph, rewrite, runner, and
  explanation pipelines before adding optional extensions

## Current shape

The implementation lives under:

- `include/Solvers/EGraph/`
- `lib/Solvers/EGraph/`

The primary umbrella header is:

- `Solvers/EGraph.h`

## Test coverage

Unit tests for the new library live under `tests/unit/Solvers/`:

- `EGraphCoreTest.cpp`
- `EGraphSimpleTest.cpp`
- `EGraphMathTest.cpp`
- `EGraphFeatureTest.cpp`

These tests cover:

- congruence closure and rebuild
- simple rewrite-driven simplification
- analysis-aware constant folding
- repeated-variable patterns
- multipattern plumbing
- explanation/dot/version surfaces

## Removed surface

`egg`'s optional LP extraction module is currently not exposed by the C++
port. The previous implementation diverged from upstream solver/backend
semantics and added a mandatory Z3 dependency to the public header surface.
Until there is a faithful replacement, use the greedy `Extractor`.
