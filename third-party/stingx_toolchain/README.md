# Vendored Stingx Toolchain

This directory contains a local copy of the `stingx` backend source used by
the scripts in this repository.

Contents:

- `main.cpp`: standalone driver that parses a `.in` file and invokes the
  invariant-generation routines.
- `CMakeLists.txt`: local build entry for the driver and the vendored
  `stingx/` sources.
- `Dockerfile`: container environment used by
  `../run_stingx_in.sh`.
- `stingx/`: vendored backend implementation and parser sources.

This copy is used so the experimental pipeline in this repository
does not depend on an external backend path. The Python scripts call
`run_stingx_in.sh`, which builds and runs this local
toolchain.

Run through the local wrapper from the repository root:

```bash
./run_stingx_in.sh BranchStructureTransforms/stingx_no_mod_inputs/SVComp_benchmark/diamond_1-1.in
```

Or build the local toolchain directly:

```bash
cd stingx_toolchain
cmake -S . -B build
cmake --build build -j$(nproc)
./build/bin/stingx_file_runner ../BranchStructureTransforms/stingx_no_mod_inputs/SVComp_benchmark/diamond_1-1.in
```
