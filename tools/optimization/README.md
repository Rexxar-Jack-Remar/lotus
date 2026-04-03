# Optimization tools

This directory contains command-line frontends for Lotus optimization passes.

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
```

The binaries are written to `build/bin/`.

## Tool

| Tool | Purpose | Notes |
| --- | --- | --- |
| `lotus-ipo` | Driver for Lotus interprocedural IPO passes | Runs the MemorySSA/ShadowMem-backed passes in `lib/Optimization/IPO`. |
| `lotus-prefetch` | Driver for Lotus software prefetch passes | Runs the software prefetching pass in `lib/Optimization/Prefetch`. |

## Input and output

The optimization tools consume LLVM bitcode or textual IR and write either
bitcode or LLVM assembly.

```bash
clang -emit-llvm -c test.c -o test.bc
build/bin/lotus-ipo test.bc -o out.bc
build/bin/lotus-ipo test.bc -S -o out.ll
build/bin/lotus-prefetch test.bc -S -o prefetch.ll
```

## IPO passes

- `--ip-all` enables every IPO pass.
- `--ipdse` runs interprocedural dead store elimination.
- `--ip-rle` runs interprocedural redundant load elimination.
- `--ip-sink` runs interprocedural store sinking.
- `--ip-forward` runs interprocedural store-to-load forwarding.

## Prefetch pass

- `lotus-prefetch` always runs `SWPrefetchingLLVMPass`.
- Prefetch-distance selection is controlled by the pass-level flags already
  declared in `lib/Optimization/Prefetch`, such as `--prefetch-distance-provider`,
  `--profile`, `--dist`, and `--llm-dist`.
- When `--prefetch-distance-provider=profile` is used, pass a sample-profile
  file with `--profile=<file>`.

## Examples

```bash
# Run the full interprocedural optimization bundle
build/bin/lotus-ipo test.bc --ip-all -o opt.bc

# Run a selected subset of IPO passes
build/bin/lotus-ipo test.bc \
  --ipdse \
  --ip-forward \
  -o opt-subset.bc

# Run software prefetching with explicit LBR distances
build/bin/lotus-prefetch test.bc \
  --prefetch-distance-provider=lbr \
  --dist=32 \
  -o prefetch.bc

# Run profile-guided software prefetching
build/bin/lotus-prefetch test.bc \
  --prefetch-distance-provider=profile \
  --profile=perf.prof \
  -o prefetch-profile.bc
```

## Related documentation

- `lib/Optimization/README.md` documents the optimization libraries.
- `lib/Optimization/IPO/README.md` documents the passes exposed by `lotus-ipo`.
- `lib/Optimization/Prefetch/` contains the software prefetching implementation
  used by `lotus-prefetch`.
