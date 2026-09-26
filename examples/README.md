# Lotus C++ SDK API Examples

This directory contains standalone C++ API examples demonstrating how to use Lotus program analysis libraries directly in your own tools and passes.

## Layout

- **`alias/`** — Alias and pointer analysis API usage:
  - `PointerAnalysisExample.cpp`: Demonstrates querying alias results (`AliasAnalysisWrapper`, `MayAlias`, `MustAlias`, etc.) on LLVM bitcode across different analysis backends.
- **`ir/`** — Intermediate representation API usage:
  - `PDGExample.cpp`: Demonstrates constructing Program Dependence Graphs (PDG), Control Dependency Graphs (CDG), and Data Dependency Graphs (DDG), and inspecting nodes and dependency edges.

## Building Examples

Examples are built via CMake when `-DLOTUS_BUILD_EXAMPLES=ON`:

```bash
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug -DLOTUS_BUILD_EXAMPLES=ON
cmake --build . --target PDGExample PointerAnalysisExample
```

The resulting binaries will be placed in `build/bin/examples/`.

## Running Examples

```bash
# Analyze a bitcode file with PDG API example
./build/bin/examples/PDGExample input.bc

# Query alias analysis information across functions
./build/bin/examples/PointerAnalysisExample input.bc
```
