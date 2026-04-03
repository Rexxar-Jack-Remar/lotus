# Alias analysis tools

This directory contains command-line frontends for the alias, points-to, and
indirect-call analyses implemented in `lib/Alias/`.

## Build

Build Lotus normally to get the static alias-analysis tools:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
```

The resulting binaries are written under `build/bin/`.

Dynamic alias-analysis tools are optional and are only built when
`BUILD_DYNAA=ON`:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DBUILD_DYNAA=ON
cmake --build build -j
```

## Input format

Most tools in this directory consume LLVM bitcode or textual LLVM IR.

```bash
clang -emit-llvm -c test.c -o test.bc
build/bin/<tool> test.bc
```

Use `build/bin/<tool> --help` to see the full option set for a specific tool.

## Tools

| Tool | Purpose | Notes |
| --- | --- | --- |
| `aser-aa` | Run AserPTA pointer analysis | Inclusion-based analysis with selectable context sensitivity (`ci`, `1-cfa`, `2-cfa`, `origin`) and solver (`basic`, `wave`, `deep`). |
| `sparrow-aa` | Run SparrowAA / Andersen analysis | Flow-insensitive subset-based analysis with configurable call-site sensitivity via `--andersen-k-cs`. |
| `lotus-aa` | Run LotusAA | Native Lotus interprocedural pointer analysis; use `-lotus-print-pts` or `-lotus-print-cg` for detailed output. |
| `dyck-aa` | Run DyckAA | Unification-based analysis; can print call-graph statistics with `--print-cg`. |
| `tpa` | Run TPA | Semi-sparse, flow- and context-sensitive pointer analysis with optional prepass dumping and CFG `.dot` output. |
| `fpa` | Run function-pointer analysis | Indirect-call target analysis with FLTA, MLTA, MLTA+DF, and KELP modes. |
| `dfpa` | Run demand-refined FPA | Refines indirect-call targets using demand-driven analysis on top of coarse candidates. |
| `call-graph` | Build a call graph with a selected backend | Supports `dyck`, `lotus`, `dfpa`, several `fpa-*` modes, and `aserpta-*` modes. |
| `sea-dsa-dg` | Dump Sea-DSA memory graphs | Useful for inspecting per-function memory graphs; enable graph emission with `--sea-dsa-dot`. |
| `seadsa-tool` | Run extended Sea-DSA utilities | Includes memory-graph dumping and other Sea-DSA related driver options. |
| `dynaa-instrument` | Instrument a program for dynamic alias logging | Built only with `BUILD_DYNAA=ON`. |
| `dynaa-check` | Compare dynamic logs against a static AA | Built only with `BUILD_DYNAA=ON`. |
| `dynaa-log-dump` | Decode `pts.log` files | Built only with `BUILD_DYNAA=ON`. |
| `dynaa` | Dynamic alias-analysis runtime driver | Built only with `BUILD_DYNAA=ON`; see `tools/alias/dynaa/README.md`. |

## Common workflows

### Compare static pointer analyses

```bash
build/bin/aser-aa test.bc --analysis-mode=1-cfa --solver=wave
build/bin/sparrow-aa test.bc --andersen-k-cs=1 --print-pts
build/bin/tpa test.bc --k-limit=1 --print-indirect-calls
```

### Inspect indirect-call targets

```bash
build/bin/fpa test.bc --analysis-type=2
build/bin/dfpa test.bc --output-file=cout
build/bin/call-graph test.bc --cg-type=lotus --emit-cg-as-json
```

### Dump graph artifacts

```bash
build/bin/tpa test.bc --cfg-dot-dir out/cfg
build/bin/sea-dsa-dg test.bc --sea-dsa-dot
build/bin/dyck-aa test.bc --print-cg
```

## Tool-specific notes

- `aser-aa` and `tpa` may consult external pointer-spec/config files. When in
  doubt, run them from the repository root so bundled config files under
  `config/` are found.
- `tpa` looks for `ptr.spec` under `LOTUS_CONFIG_DIR`, then `config/ptr.spec`
  relative to the current working directory.
- `lotus-aa` prints only a completion message unless LotusAA-specific flags such
  as `-lotus-print-pts` or `-lotus-print-cg` are enabled.
- `call-graph` emits DOT by default and can also emit JSON with
  `--emit-cg-as-json`.
- Sea-DSA tooling depends on the Sea-DSA integration being available in the
  current build.

## Related documentation

- `lib/Alias/README.md` compares the analyses implemented in Lotus.
- `tools/alias/dynaa/README.md` documents the dynamic alias-analysis workflow.
