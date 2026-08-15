# Native C++ Datalog semantics

This document freezes the semantic contract for Lotus's native Datalog engine.
The public API is a strongly typed C++17 API. It is not a parser or a compatibility
layer for Ascent's Rust macro syntax.

## Architecture boundary

Every program crosses the following boundary before it executes:

```text
typed C++ API -> semantic IR -> analyzer/execution plan -> runtime
```

The template API only constructs typed terms and relations. Rules are immediately
lowered to type-erased `RuleIR`, `AtomIR`, `TermIR`, and `FilterIR` values. Runtime
storage and rule evaluation do not depend on the public template expression tree.

## Frozen language semantics

### Relations

A relation is a set of tuples. Inserting an existing tuple has no effect. Column
types and arity are fixed when the relation is created. Values must support
`operator==` and `std::hash`.

### Rules and grounding

The planner may reorder positive atoms, filters, and negations while preserving
aggregate boundaries. A positive body atom grounds every variable it contains.
Repeated occurrences of a variable impose an equality constraint. Constants impose
equality constraints on their columns.

Every variable used by a filter, head term, or head expression must already be
grounded. Invalid programs fail in `program::compile()`; they do not fail during
fixed-point execution.

### Expressions and conditions

Expressions are side-effect-free C++ values evaluated from the current variable
binding. A `where(...)` condition succeeds only when its boolean expression is true.
Expression terms are allowed in rule heads. Body relation terms are variables,
wildcards, or constants; computed body constraints use `where(...)`.

### Recursion

Positive relation dependencies are condensed into strongly connected components.
Acyclic SCCs execute once. Recursive SCCs execute to a least fixed point using
`Total`, `Delta`, and `New` epochs. During an epoch, `Total` is immutable; facts are
deduplicated and merged only at the epoch boundary.

For a recursive rule, the executor creates one semi-naive variant per recursive
body atom. That occurrence reads `Delta`; the remaining occurrences read `Total`.

### Indexes

At an atom, columns containing constants or variables grounded by earlier body
items form a runtime bitmask lookup key. Indexes are created lazily and rebuilt when
their relation version changes. An atom with no bound columns performs a full scan.

### Aggregation

Aggregation is stratified. An aggregate dependency requires the head stratum to be
strictly greater than the input stratum. Generic blocking aggregators consume an
input range. Reducible aggregators additionally expose local state, merge, and
finish operations for parallel execution.

The built-in reducible aggregators are `sum`, `count`, `minimum`, `maximum`, and
`mean`. `make_aggregator` constructs a generic blocking aggregator that may emit
zero, one, or multiple results.

### Lattices

A lattice relation maps a tuple key to one lattice value. Insertion for an existing
key performs `join`; it produces new information only when the stored value changes.
All candidates for a key are joined before that key enters the next delta.

The standard library provides minimum, maximum, and set-union lattices. User-defined
lattice values provide `bool joinMut(const T &candidate)`.

### Negation

Negation is a stratified anti-join. A negative dependency requires the head stratum
to be strictly greater than the referenced relation's stratum. Every variable in a
negated atom must be grounded before the atom is evaluated.

### Parallel execution

Parallel evaluation uses bulk-synchronous epochs: workers read immutable `Total`
and `Delta`, write thread-local candidates, and merge after a barrier. Ordinary
relations deduplicate candidates; lattice relations locally join by key before the
global join.

The runtime accepts an injected `Scheduler`. `ThreadScheduler` supplies the default
`std::thread` implementation and `SerialScheduler` provides deterministic single-
thread execution.

## Explicit non-goals

- Rust macro or Rust DSL compatibility
- `ascent_source!` and Rust host-expression parsing
- procedural macros or generated C++ kernels
- BYODS implementations (an extension boundary may be added later)
- WASM support
- incremental deletion
- distributed execution

## Support matrix

| Feature | Status | Semantics |
| --- | --- | --- |
| Typed relations, variables, constants | implemented | compile-time API checks |
| Conditions and head expressions | implemented | grounded pure expressions |
| Positive recursion and SCCs | implemented | least fixed point |
| Semi-naive execution | implemented | `Total` / `Delta` / `New` epochs |
| Runtime bitmask indexes | implemented | lazy indexes from bound columns |
| Aggregation | implemented | stratified generic/reducible aggregators |
| Lattice relations | implemented | one joined value per key |
| Negation | implemented | stratified anti-join |
| Join planning | implemented | greedy cardinality estimate |
| Parallel execution | implemented | bulk-synchronous scheduler |
| Runtime tracing | implemented | SCC, rule, and delta traces |

## Differential-test contract

Semantic parity tests should run equivalent programs in Rust Ascent and the C++
engine, canonicalize tuple order, and compare results. The suite is grouped by
relations, recursion, aggregation, lattices, negation, parallel execution, and
invalid programs. The portable harness always checks the C++ output against a
golden result and optionally builds the Rust runner when `LOTUS_ASCENT_SOURCE_DIR`
points to an Ascent checkout.
