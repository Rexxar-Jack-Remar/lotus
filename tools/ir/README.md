# IR tools

This directory contains command-line frontends for LLVM-based intermediate
representations built in `lib/IR/`.

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
```

The current IR tool binary is emitted under `build/bin/`.

## Tools

| Tool | Purpose | Notes |
| --- | --- | --- |
| `pdg-query` | Query the Program Dependence Graph | Implemented by `tools/ir/lotus-ir-pdg-query.cpp`; supports Cypher-style queries, slicing, chopping, shortest paths, summaries, resource-flow queries, and multiple output formats. |

## Typical usage

`pdg-query` consumes LLVM bitcode or textual LLVM IR:

```bash
clang -emit-llvm -c test.c -o test.bc
build/bin/pdg-query test.bc --query "MATCH (n) RETURN n LIMIT 5"
```

Useful options include:

- `--query` / `--query-file` to execute Cypher queries.
- `--interactive` to start an interactive query session.
- `--analysis` to run built-in PDG analyses such as `slice-forward`,
  `slice-backward`, `chop`, `shortest-path`, `impact`, or `resource-flow`.
- `--format=text|json|dot` to control output formatting.
- `--property-file` with `--direction` for property-driven slicing.
- `--edge-preset` and `--context-sensitive` to tune traversal behavior.

## Examples

```bash
# Run a single query
build/bin/pdg-query test.bc --query "MATCH (f:Function) RETURN f.name"

# Compute a backward slice from criteria selected by a query
build/bin/pdg-query test.bc \
  --analysis=slice-backward \
  --criteria-query "MATCH (n {name:'x'}) RETURN n"

# Dump JSON output for scripting
build/bin/pdg-query test.bc --query-file tools/ir/examples/dataflow.cypher --format=json
```

## Related documentation

- Query examples live in `tools/ir/examples/README.md` and `tools/ir/examples/`.
- PDG implementation details live in `lib/IR/PDG/README.md`.
