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

## Design goals

- no Z3 dependency in the public API
- reusable outside SMT-specific clients
- small, header-driven C++ surface compatible with Lotus build conventions
- enough parity with `egg`'s core model to support direct migration of simple
  rewrite workloads

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
