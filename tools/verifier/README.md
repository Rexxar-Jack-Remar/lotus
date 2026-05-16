# Verifier tools

This directory contains verification-oriented command-line frontends built on
top of `lib/Verification/` and imported verifier integrations.

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
```

Several verifier families are optional:

- `lotus-verify-sifa` and `lotus-verify-symabs-ai` are configured by default.
- CLAM tools are built only when `LOTUS_ENABLE_CLAM=ON`.
- SeaHorn tools are built only when `LOTUS_ENABLE_SEAHORN=ON`.
- `hice-dt` is built only when `LOTUS_ENABLE_HORN_ICE=ON`.

## Tool families

| Tool / family | Availability | Purpose |
| --- | --- | --- |
| `lotus-verify-sifa` | default | Symbolic Interpretation with Fluid Abstractions for reachability and invariant inference. |
| `lotus-verify-symabs-ai` | default | Abstract-interpretation driver with configurable domains, fragmentation, and memory models. |
| `clam`, `clam-diff`, `clam-pp` | `LOTUS_ENABLE_CLAM=ON` | CLAM-based abstract interpretation, preprocessing, and JSON differencing. |
| `seahorn`, `seapp`, `seainspect` | `LOTUS_ENABLE_SEAHORN=ON` | SeaHorn verification, preprocessing, and inspection tools. |
| `hice-dt` | `LOTUS_ENABLE_HORN_ICE=ON` | ICE-style learning for Horn clauses / Boogie workflows. |

## Default tools

### `lotus-verify-sifa`

`lotus-verify-sifa` analyzes a selected function in LLVM bitcode and can run either the fast
instruction-level transfer semantics or the SMT-backed SymbolicAbstraction
backend.

Implementation: `tools/verifier/sifa/lotus-verify-sifa.cpp`

```bash
build/bin/lotus-verify-sifa input.bc --function main --abstract-domain Interval
build/bin/lotus-verify-sifa input.bc --symabs --abstract-domain Octagon --reachability
```

See `tools/verifier/sifa/README.md` for detailed options.

### `lotus-verify-symabs-ai`

`lotus-verify-symabs-ai` is a configurable abstract-interpretation frontend with pluggable
domains and configuration files.

Implementation: `tools/verifier/symabs-ai/lotus-verify-symabs-ai.cpp`

```bash
build/bin/lotus-verify-symabs-ai --list-domains
build/bin/lotus-verify-symabs-ai input.bc --function main --abstract-domain Interval
build/bin/lotus-verify-symabs-ai input.bc --config config/symabs-ai/default.conf --check-assertions
```

Useful options include `--list-configs`, `--fragment-strategy`,
`--memory-model`, `--widening-delay`, and `--check-memsafety`.

## Optional integrations

- CLAM documentation is in `tools/verifier/clam/README.md`.
- SeaHorn runtime notes are in `tools/verifier/seahorn/sea-rt/README.md`.
- `hice-dt` is the executable built from `third-party/horn-ice/hice-dt/`.

## Related documentation

- `lib/Verification/README.md` documents the verification libraries.
- `config/symabs-ai/README.md` explains SymAbsAI configuration files.
