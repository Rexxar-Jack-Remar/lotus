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

## Typed languages

Typed languages use an X-macro variant list. The declaration order is also the
parse priority, so literal operators should precede payload fallbacks.

```cpp
#define MATH_VARIANTS(V)                                                      \
  V(CONSTANT, Pi, "pi", _)                                                    \
  V(FIXED, Add, "+", 2)                                                       \
  V(VARIADIC, List, "list", _)                                                \
  V(DATA, Number, _, int64_t)                                                 \
  V(DATA_VARIADIC, Other, _, lotus::egraph::Symbol)

LOTUS_EGRAPH_DEFINE_TYPED_LANGUAGE_IN(math, MathLang, MATH_VARIANTS);
```

This generates variant-specific factories and accessors such as
`MathLang::makeNumber(42)`, `node.isAdd()`, and `node.getNumber().value`.
The base macro may also be invoked directly inside an existing namespace. Use
`LanguageHash<MathLang>` when a typed language is used as a hash-table key.

`TypedValueCodec<T>` can be specialized for payloads that do not support stream
parsing and formatting. Boolean and integral codecs follow Rust `FromStr`
semantics, including exact `true`/`false` parsing and checked integer ranges.
`DATA_FIXED` combines a payload with a fixed number of children using
`LOTUS_EGRAPH_TYPED_DATA(PayloadType, arity)`.

Payloads must be copyable, equality comparable, ordered, and hashable. Raw
floating-point payloads are rejected because NaN does not satisfy the e-graph's
equality and ordering invariants; use an ordered/non-NaN wrapper with a codec.
Generated languages have no default or invalid node state and can only be
constructed through their variant factories.

## Lotus compatibility APIs and extensions

The port keeps several Lotus-specific conveniences that do not correspond
one-for-one with upstream Rust APIs:

- helper constructors such as `makeRewrite`, `makeConditionalRewrite`,
  `makeMultiRewrite`, and borrowed rewrite builders
- `LOTUS_EGRAPH_DEFINE_TYPED_LANGUAGE`, which defines variant-discriminated
  languages with constants, typed payloads, fixed-arity nodes, variadic nodes,
  and payload-bearing operators
- JSON import/export helpers for `EGraph`; typed nodes include their variant
  names so serialization preserves variants with overlapping display strings
- `egraphUnion`, `egraphIntersect`, and language-mapper utilities

These APIs are supported as Lotus extensions, but they should not be read as a
claim of full public API parity with `egg`.

## Unsupported upstream functionality

Some upstream `egg` functionality is still intentionally out of scope:

- no LP extractor surface: the previous Z3-backed DAG extractor was not a
  faithful migration of `egg`'s `good_lp`-based implementation and has been
  removed pending a proper redesign
- no macro-level Rust API parity for `rewrite!` or `multi_rewrite!`
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
