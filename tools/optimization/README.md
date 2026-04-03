# Optimization tools

This directory contains command-line frontends for optimization and
verification-oriented transformation passes.

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
```

The binary is written to `build/bin/lotus-opt`.

## Tool

| Tool | Purpose | Notes |
| --- | --- | --- |
| `lotus-opt` | Driver for Lotus optimization and transformation passes | Combines interprocedural optimizations, canonicalization, instrumentation, and verifier-oriented preprocessing passes. |

## Input and output

`lotus-opt` consumes LLVM bitcode or textual IR and writes either bitcode or
LLVM assembly.

```bash
clang -emit-llvm -c test.c -o test.bc
build/bin/lotus-opt test.bc -o out.bc
build/bin/lotus-opt test.bc -S -o out.ll
```

## Major pass groups

- Interprocedural optimization: `--ip-all`, `--ainline`, `--ipdse`,
  `--ip-rle`, `--ip-sink`, `--ip-forward`.
- Verifier preparation: `--prep-overflows`, `--delete-undefined`,
  `--remove-error-calls`, `--rename-verifier-funs`,
  `--replace-verifier-atomic`.
- Memory and allocation instrumentation: `--instrument-alloc`,
  `--instrument-alloc-nf`, `--replace-lifetime-markers`, `--init-uninit`.
- Loop and CFG normalization: `--break-crit-loops`, `--flatten-loops`,
  `--remove-infinite-loops`, `--break-infinite-loops`, `--lotus-loop-unroll`.
- Inspection utilities: `--classify-instructions`, `--classify-loops`,
  `--count-instr`, `--check-module`, `--get-test-targets`.

## Examples

```bash
# Run the full interprocedural optimization bundle
build/bin/lotus-opt test.bc --ip-all -o opt.bc

# Prepare code for downstream verification
build/bin/lotus-opt test.bc \
  --prep-overflows \
  --delete-undefined \
  --remove-error-calls \
  -o prep.bc

# Collect module statistics without rewriting the program
build/bin/lotus-opt test.bc --count-instr --classify-loops
```

## Related documentation

- `lib/Optimization/README.md` documents the optimization libraries.
- `lib/Transform/README.md` documents transformation infrastructure used by
  several `lotus-opt` passes.
