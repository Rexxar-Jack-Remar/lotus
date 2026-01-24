# PDG Cypher query examples

These files are meant to be run with `pdg-query` (one query per line; `#` starts a comment).

## Run

- Build: `cmake --build build --target pdg-query`
- Execute a query file: `./build/bin/pdg-query -f tools/ir/examples/metrics.cypher <input.bc>`
- New primitives demo: `./build/bin/pdg-query -f tools/ir/examples/primitives.cypher <input.bc>`

For metric-style queries where you want the full count, run with `--limit 0` (otherwise `Result(N nodes)` reflects the limited result set).

By default, `pdg-query` runs the `ProgramDependencyGraph` pass (`--build-pdg`) so edge queries (data/control/param/call) work.

## Mini schema (Lotus PDG)

**Labels**
- `:INST` (all instruction nodes), `:INST_FUNCALL`, `:INST_RET`, `:INST_BR`, `:INST_OTHER`
- `:FUNC_ENTRY`
- `:PARAM` (all parameter-tree nodes), `:PARAM_FORMALIN`, `:PARAM_FORMALOUT`, `:PARAM_ACTUALIN`, `:PARAM_ACTUALOUT`
- `:VAR` (all variable nodes; includes globals), plus the specific `:VAR_*` labels
- `:ANNO` (annotation nodes), plus `:ANNO_VAR`, `:ANNO_GLOBAL`, `:ANNO_OTHER`

**Edges (for `-[:TYPE]->`)**
- `:DATA_DEP` (def-use), `:DATA_RAW`, `:DATA_READ`, `:DATA_ALIAS`
- `:CONTROL_DEP`
- `:CALL_INV`, `:CALL_RET`, `:IND_CALL`
- `:PARAM_IN`, `:PARAM_OUT`

## Extended query primitives

- **Directional traversals:** `-[:T]->` follows outgoing edges; `<-[:T]-` follows incoming edges; `-[:T]-` or `<-[:T]->` follows both.
- **Multi-edge-type traversals:** `:T1|T2` matches either edge type (e.g., `:DATA_DEP|CONTROL_DEP`).
- **List filters:** `WHERE n.opcode IN ["load","store"]`.
- **Aggregation:** `RETURN COUNT(*)`, `COUNT(n)`, `COUNT(DISTINCT n.prop)`.
- **Parameters:** pass `--param key=value` and reference as `$key` (works in `{prop:$key}` and `WHERE`).

**Common node properties (usable in `WHERE` and `RETURN`)**
- `func`, `label`
- `opcode` (LLVM opcode for instruction nodes)
- `callee` (for `:INST_FUNCALL`; returns `"<indirect>"` for indirect calls)
- `src`, `src_file`, `src_line`, `src_col` (requires debug info in the input bitcode)
- `llvm` (LLVM IR string for the underlying value/instruction)

**Common edge properties**
- `label`, `src_*`, `dst_*` (e.g., `e.label`, `e.src_func`, `e.dst_src`)
