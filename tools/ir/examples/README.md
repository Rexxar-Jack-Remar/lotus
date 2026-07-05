# PDG Cypher query examples

These files are meant to be run with `lotus-ir-pdg-query` (one query per line; `#` starts a comment).

## Run

- Build: `cmake --build build --target lotus-ir-pdg-query`
- Execute a query file: `./build/bin/lotus-ir-pdg-query -f tools/ir/examples/metrics.cypher <input.bc>`
- New primitives demo: `./build/bin/lotus-ir-pdg-query -f tools/ir/examples/primitives.cypher <input.bc>`

For metric-style queries where you want the full count, run with `--limit 0` (otherwise `Result(N nodes)` reflects the limited result set).

By default, `lotus-ir-pdg-query` runs the `ProgramDependencyGraph` pass (`--build-pdg`) so edge queries (data/control/param/call) work.

## PDG schema introspection

Instead of relying on this mini schema, get the complete up-to-date schema as JSON:

```bash
./build/bin/lotus-ir-pdg-query --schema
```

This prints all node labels, edge types, node/edge properties, group labels, and edge presets. LLM agents should call this before writing Cypher queries.

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

---

## Security queries

The `security/` directory contains categorized security analysis patterns.

| File | Patterns | Analysis modes |
|------|----------|----------------|
| `security/injection.cypher` | Command injection: system/popen/exec sinks + input source tracing | Cypher API scan + `--analysis chop` |
| `security/memory.cypher` | Use-after-free, double-free, memory leaks | `--analysis resource-flow`, `--analysis chop`, `--analysis shortest-path` |
| `security/unsafe-libc.cypher` | strcpy/gets/sprintf, format string, buffer overflow | Cypher API scan + `--analysis chop` + backward slice |
| `security/resource.cypher` | File handle leaks, lock/unlock, mmap/munmap | `--analysis resource-flow`, `--analysis summary` |
| `security/double-free.cypher` | Double-free, use-after-free, new/delete mismatch | `--analysis resource-flow`, `--analysis chop`, `--analysis shortest-path` |
| `security/taint.cypher` | Input-to-sink taint tracking, format string, argument tracing | Cypher API scan + `--analysis chop` + `--analysis slice-backward` |

Each file documents the prerequisites and CLI invocation for each pattern.

### Quick-start security triage

```bash
# 1. Find all shell execution call sites
./build/bin/lotus-ir-pdg-query input.bc -f tools/ir/examples/security/injection.cypher

# 2. Detect heap memory leaks (malloc without free)
./build/bin/lotus-ir-pdg-query input.bc \
  --analysis resource-flow \
  --criteria-query "MATCH (c:INST_FUNCALL) WHERE c.callee IN ['malloc','calloc','realloc'] RETURN c" \
  --resource-kind heap \
  --format json

# 3. Detect file handle leaks (fopen without fclose)
./build/bin/lotus-ir-pdg-query input.bc \
  --analysis resource-flow \
  --criteria-query "MATCH (c:INST_FUNCALL) WHERE c.callee = 'fopen' RETURN c" \
  --resource-kind file \
  --format json

# 4. Find strcpy/gets call sites
./build/bin/lotus-ir-pdg-query input.bc -f tools/ir/examples/security/unsafe-libc.cypher
```

### Source-sink data flow (chop analysis)

For tracing data flow from input sources to security-sensitive sinks:

```bash
./build/bin/lotus-ir-pdg-query input.bc \
  --analysis chop \
  --criteria-query "MATCH (s:INST_FUNCALL) WHERE s.callee IN ['fgets','read','scanf','getenv','recv'] RETURN s" \
  --target-query "MATCH (t:INST_FUNCALL) WHERE t.callee IN ['system','popen','execve'] RETURN t" \
  --edge-preset value-flow \
  --context-sensitive \
  --format json
```

The `witness_paths` field in the JSON output shows the instruction chain
connecting source to sink.
