# Lotus EGraph

`LotusEGraph` is a solver-agnostic C++17 e-graph library for Lotus.

It is modeled after the Rust `egg` library and aims to preserve `egg` semantics
for the supported core equality-saturation pipeline while keeping a Lotus-first
C++ API.

## Supported core (`egg`-aligned)

The current port aligns its tested core surface around the main `egg`
abstractions used for equality saturation and rewrite-driven normalization:

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
- explanation support
- dot export support

These surfaces are covered by the unit tests under `tests/unit/Solvers/` and
should track `egg` core behavior unless this document calls out an intentional
Lotus extension.

## Lotus compatibility APIs and extensions

The port keeps several Lotus-specific conveniences that do not correspond
one-for-one with upstream Rust APIs:

- helper constructors such as `makeRewrite`, `makeConditionalRewrite`,
  `makeMultiRewrite`, and borrowed rewrite builders
- `LOTUS_EGRAPH_DEFINE_LANGUAGE` / `LOTUS_EGRAPH_LANG_OP`, which provide a
  lightweight operator/arity-constrained language surface rather than full
  `egg::define_language!` parity
- JSON import/export helpers for `EGraph`
- `egraphUnion`, `egraphIntersect`, and language-mapper utilities

These APIs are supported as Lotus extensions, but they should not be read as a
claim of full public API parity with `egg`.

## Unsupported upstream functionality

Some upstream `egg` functionality is still intentionally out of scope:

- no LP extractor surface: the previous Z3-backed DAG extractor was not a
  faithful migration of `egg`'s `good_lp`-based implementation and has been
  removed pending a proper redesign
- no macro-level Rust API parity such as `rewrite!`, `multi_rewrite!`, or the
  full `define_language!` feature set
- no attempt to mirror Rust-only crate structure such as tutorials, doctests,
  or feature gating

## Design goals

- reusable outside SMT-specific clients
- small, header-driven C++ surface compatible with Lotus build conventions
- preserve `egg` semantics for the tested core e-graph, rewrite, runner, and
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
- explanation, dot, extraction, and runner behavior
- Lotus extension surfaces such as JSON round-tripping and egraph
  union/intersection helpers
