# Install Lotus

## Prerequisites

- LLVM 14.x
- Z3 4.11
- CMake 3.10+
- C++17 compatible compiler

## Quick Build

```bash
git clone https://github.com/ZJU-PL/lotus
cd lotus
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
```

Run tests:

```bash
ctest --output-on-failure
```

## Build Configuration

Lotus exposes its build options through `cmake/LotusOptions.cmake` with a consistent `LOTUS_*` naming scheme. CMake prints a build summary after configuration so the enabled surface is explicit.

### Common Options

| Option | Default | Description |
|--------|---------|-------------|
| `LOTUS_BUILD_TESTS` | OFF | Build unit tests |
| `LOTUS_BUILD_EXAMPLES` | OFF | Build examples |
| `LOTUS_ENABLE_CLAM` | OFF | CLAM abstract interpretation |
| `LOTUS_ENABLE_SEAHORN` | OFF | SeaHorn verification |
| `LOTUS_ENABLE_SMACK` | OFF | SMACK verification |
| `LOTUS_ENABLE_HORN_ICE` | OFF | Horn-ICE tools |
| `LOTUS_ENABLE_DYNAA` | OFF | Dynamic alias-analysis tools |
| `LOTUS_ENABLE_CFL` | OFF | CFL reachability |
| `LOTUS_ENABLE_CSR` | OFF | CSR tools |
| `LOTUS_ENABLE_OWL` | OFF | OWL integration |
| `LOTUS_DOWNLOAD_BOOST` | OFF | Auto-download Boost |
| `LOTUS_DOWNLOAD_CRAB` | OFF | Auto-download Crab |

### Example Configurations

```bash
# Default lean development build (no tests or CLAM)
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug

# Full build, including tests and CLAM
cmake -S . -B build-full -G Ninja \
  -DLOTUS_BUILD_TESTS=ON -DLOTUS_ENABLE_CLAM=ON

# Enable dynamic alias analysis
cmake -S . -B build -DLOTUS_ENABLE_DYNAA=ON

# Enable Horn-ICE and CFL tools
cmake -S . -B build -DLOTUS_ENABLE_HORN_ICE=ON -DLOTUS_ENABLE_CFL=ON \
  -DLOTUS_ENABLE_CSR=ON

# Disable heavyweight verifiers
cmake -S . -B build -DLOTUS_ENABLE_CLAM=OFF -DLOTUS_ENABLE_SEAHORN=OFF \
  -DLOTUS_ENABLE_SMACK=OFF

# Custom dependency paths
cmake -S . -B build \
  -DLOTUS_CUSTOM_BOOST_ROOT=/path/to/boost \
  -DLOTUS_CUSTOM_CRAB_ROOT=/path/to/crab
```

### Custom LLVM Path

If CMake cannot find LLVM automatically:

```bash
cmake .. -DLLVM_BUILD_PATH=/path/to/llvm/lib/cmake/llvm
```

### Boost Dependencies

Boost is optional and only required when certain modules are enabled:

- **SeaHorn** (`LOTUS_ENABLE_SEAHORN`) — expression handling, Horn clause DB
- **CLAM** (`LOTUS_ENABLE_CLAM`) — abstract interpretation, JSON parsing (Boost 1.80+)
- **CclyzerAA** (`LOTUS_USE_CCLYZER`) — alias analysis
- **FPsolve** (`LOTUS_ENABLE_FPSOLVE`) — fixed-point solver; also requires GMP

If all four are disabled, Boost will not be configured. Example:

```bash
cmake -S . -B build -DLOTUS_ENABLE_CLAM=OFF -DLOTUS_ENABLE_SEAHORN=OFF \
  -DLOTUS_ENABLE_FPSOLVE=OFF -DLOTUS_USE_CCLYZER=OFF
```

When Boost is needed, the build will download and build it if not found. You can specify a custom path with `-DLOTUS_CUSTOM_BOOST_ROOT=/path/to/boost`.
