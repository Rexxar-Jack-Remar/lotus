# lotus-datalog

`lotus-datalog` is the command-line entry point for the native Lotus
Datalog/lattice engine. It consumes JSON Semantic IR, rather than a benchmark name
or the Rust Ascent DSL, and writes deterministic JSON results.

```text
lotus-datalog run <program.json|-> [options]
lotus-datalog validate <program.json|->
lotus-datalog schema
```

`schema` prints a runnable transitive-closure example. `validate` performs all
compile-time analysis without running the fixed point. `run` accepts
`--workers N`, `--grain-size N`, `--pretty`, `--trace-scc`, `--trace-rule`, and
`--trace-delta`.

## Program shape

```json
{
  "relations": [
    {
      "name": "edge",
      "columns": ["i64", "i64"],
      "kind": "relation",
      "facts": [[1, 2], [2, 3]]
    },
    {
      "name": "path",
      "columns": ["i64", "i64"]
    }
  ],
  "rules": [
    {
      "head": {"relation": "path", "args": ["$x", "$y"]},
      "body": [
        {"atom": {"relation": "edge", "args": ["$x", "$y"]}},
        {"where": {"op": ">", "args": ["$y", 0]}}
      ]
    }
  ],
  "outputs": ["path"]
}
```

Supported scalar column types are `i64`, `u64`, `f64`, `string`, and `bool`.
Lattice value types are `min<i64>`, `max<i64>`, `min<f64>`, `max<f64>`, and
`set<i64>`. A lattice relation must use a lattice type for its final column; all
preceding columns form the key.

Variables are strings beginning with `$`. `_` is an anonymous variable. Constants
may be written directly or as `{"const": value}`. An atom has the form
`{"relation": "name", "args": [...]}`.

## Rule bodies

Body entries are one of:

```json
{"atom": {"relation": "edge", "args": ["$x", "$y"]}}
{"not": {"relation": "blocked", "args": ["$y"]}}
{"where": {"op": ">", "args": ["$y", 0]}}
{"aggregate": {
  "output": "$sum",
  "function": "sum",
  "value": "$weight",
  "source": {"relation": "weighted", "args": ["$key", "$weight"]}
}}
```

Rules use either `head` or `heads`. Supported aggregate functions are `count`,
`sum`, `min`, `max`, and `mean`. The aggregate source is one atom; variables bound
before the aggregate act as group keys. Negated atoms and filter expressions must
refer only to variables grounded by earlier body entries.

Expressions use `{"op": operator, "args": [...]}`. Numeric operators are `+`,
`-`, `*`, `/`, `%`, `==`, `!=`, `<`, `<=`, `>`, and `>=`; booleans additionally
support `!`, `&&`, and `||`. Unary numeric operators are `unary-` and `unary+`.
`min_lattice`, `max_lattice`, and `set_lattice` construct lattice values.

The output contains requested relation rows in canonical order and execution
statistics. Parallel statistics distinguish rule, merge, and aggregate tasks. This
stable interface is intended for later Python-based semantic differential tests and
performance measurement.

The JSON frontend intentionally exposes a portable fixed set of scalar and lattice
types. Native C++ programs can additionally use custom types, streaming and custom
reducible aggregators, and the dual, product, bounded-set, and
constant-propagation lattice classes from `Lattice.h`.
