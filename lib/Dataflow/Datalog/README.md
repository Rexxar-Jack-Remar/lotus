# Native C++ Datalog engine

This document describes the current semantic contract and architecture of Lotus's
native Datalog engine. It evolves together with the implementation.
The public API is a strongly typed C++17 API. It is not a parser or a compatibility
layer for Ascent's Rust macro syntax.

## Architecture boundary

Every program crosses the following boundary before it executes:

```text
typed C++ API -> semantic IR -> analyzer -> physical RulePlan -> runtime
```

The template API only constructs typed terms and relations. Rules are immediately
lowered to type-erased `RuleIR`, `AtomIR`, `TermIR`, and `FilterIR` values. Validation
and stratification consume this semantic IR once; execution uses a lowered physical
`RulePlan` with preclassified scan/filter/anti-lookup/aggregate operations, selected
lookup masks, and preprocessed head terms.

## Language semantics

### Relations

A relation is a set of tuples. Inserting an existing tuple has no effect. Column
types and arity are fixed when the relation is created. Values must support
`operator==` and `std::hash`.

Facts inserted through `Relation::insert` are base facts. Solver output is stored
separately as derived state. A later `run()` after new base facts clears derived state
from the first affected SCC onward and recomputes that suffix. This makes additive
reruns correct for positive rules, negation, aggregates, and lattices; fact deletion
is intentionally not part of the API.

`CompiledProgram` retains the underlying context state and is therefore safe to run
after the `Context` wrapper is destroyed. Fluent `Relation`, `Var`, and expression
handles remain context-bound construction handles and should not outlive the
wrapper that created them.

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
body atom. That occurrence reads `Delta`; earlier recursive occurrences read the
pre-epoch total while later occurrences may read the total including delta. This
leftmost-delta convention makes the variants disjoint instead of relying on final
set deduplication to remove duplicate derivations.

### Indexes

At an atom, columns containing constants or variables grounded by earlier body
items form a runtime bitmask lookup key. The planner's statistics catalog is
separate from physical indexes: it observes distinct-key counts without installing
an index, then the finalized `RulePlan` installs only its selected masks. Runtime
indexes are hash buckets of row IDs and are maintained incrementally on row inserts
and lattice updates. Lookups borrow binding cells through a zero-allocation key view
and stream rows directly to the consumer; an atom with no bound columns performs a
full scan.

Join costs use observed distinct-key counts, rather than a fixed heuristic based
only on the number of bound columns. During execution,
variable bindings reference immutable relation cells and own only computed values,
avoiding a `std::any` copy at every join step.

### Aggregation

Aggregation is stratified. An aggregate dependency requires the head stratum to be
strictly greater than the input stratum. `make_streaming_aggregator` exposes a
callback range without materializing all projected inputs. The compatibility
`make_aggregator` API collects a vector. Reducible aggregators expose local state,
merge, and finish operations for parallel execution.

The built-in reducible aggregators are `sum`, `count`, `minimum`, `maximum`, and
`mean`. `make_reducible_aggregator` constructs parameterized custom reducers. They
run serially unless passed `ReducerProperties::parallel()`, which explicitly attests
that `add`/`merge` are associative, commutative, deterministic, and safe to run in
parallel. Generic aggregators may emit zero, one, or multiple results.

### Lattices

A lattice relation maps a tuple key to one lattice value. Insertion for an existing
key performs `join`; it produces new information only when the stored value changes.
All candidates for a key are joined before that key enters the next delta.

The standard library provides minimum, maximum, set-union, dual, product,
bounded-set, and constant-propagation lattices. User-defined lattice values provide
`bool joinMut(const T &candidate)`; values used through `DualLattice` additionally
provide `meetMut`. Lattice joins must be associative, commutative, and idempotent.
The runtime evaluates joins on copies and commits them only after all parallel
inspection tasks complete, so a throwing join cannot leave live rows or indexes
partially updated.

### Negation

Negation is a stratified anti-join. A negative dependency requires the head stratum
to be strictly greater than the referenced relation's stratum. Every variable in a
negated atom must be grounded before the atom is evaluated.

### Parallel execution

Parallel evaluation uses bulk-synchronous epochs: workers read immutable `Total`
and `Delta`, write thread-local candidates, and merge after a barrier. Ordinary
relations deduplicate candidates; lattice relations locally join by key before the
global join.

The runtime accepts an injected `Scheduler`. `ThreadScheduler` uses a persistent
worker pool and `SerialScheduler` provides deterministic single-thread execution.
Non-recursive rules and recursive delta partitions both execute in parallel when
there is enough work. Only reducers that declare `ReducerProperties::parallel()`
use worker-local states followed by a deterministic merge; generic and undeclared
reducers remain serial. Epoch candidates are hash-partitioned for parallel set
deduplication or per-key lattice joining.

Expression lifts, hash/equality functions, and lattice joins must be pure with
respect to the relation database. The type system cannot prove that host C++
callables satisfy those laws, so they are part of the embedding contract.

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
| Runtime bitmask indexes | implemented | selected at compile time; incrementally maintained |
| Aggregation | implemented | stratified generic/reducible aggregators |
| Lattice relations | implemented | joined values plus standard lattice library |
| Negation | implemented | stratified anti-join |
| Join planning | implemented | greedy observed-distinct cost estimate |
| Parallel execution | implemented | BSP rules, reducers, coalesce, and merge |
| Runtime tracing | implemented | SCC, rule, and delta traces |

## CLI and external validation contract

`lotus-datalog` consumes a JSON Semantic IR rather than Rust syntax. It emits
canonical, sorted JSON rows plus execution statistics, so later differential and
performance testing can be implemented as ordinary Python scripts without a
dedicated C++ benchmark or `tests/differential` target. `lotus-datalog validate`
performs parsing, type checking, grounding, dependency analysis, stratification,
SCC construction, and planning without executing the fixed point.
