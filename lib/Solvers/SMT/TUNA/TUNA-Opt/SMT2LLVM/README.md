# SMT2LLVM

A bidirectional SMT-LIB 2 ↔ LLVM IR conversion toolkit that TUNA-Opt depends on, extended from [SLOT](https://github.com/TUNA-SMT/SLOT).

Core function: translate SMT-LIB 2 formulas to LLVM IR, simplify them using LLVM optimization passes, then convert back to SMT-LIB 2 to achieve structural simplification of SMT formulas.

## Directory Structure

```
SMT2LLVM/
├── include/                  # Header files
├── src/
│   ├── tools/                # main entry points for each executable
│   │   ├── main.cpp          # slot
│   │   ├── SMT2LLVM.cpp      # smt2llvm
│   │   ├── LLVM2SMT.cpp      # llvm2smt
│   │   ├── LLVM2FEAT.cpp     # llvm2feat
│   │   └── fastslot.cpp      # fastslot
│   └── ...                   # Conversion logic implementation
├── resources/                # Bug reproduction examples
├── passes-slot-old.txt       # 9 passes used by the original SLOT
├── passes-run.txt            # All 41 meaningful supported passes
├── passes-filter.txt         # 25-pass subset with clear optimization benefit
├── passes-useful.txt         # 33 optimization-related passes
├── passes-16.txt             # Base list of 80 passes available in LLVM 16
├── passes-all-llvm16.txt     # Full LLVM 16 pass list (353 passes)
└── FunctionPasses.md         # Documentation for 142 LLVM FunctionPasses
```

## Installation

### System Dependencies

```bash
sudo apt install -y git gcc g++ cmake ninja-build python3 \
                    zlib1g-dev libtinfo-dev libxml2-dev
```

### Build LLVM 16.0.0

```bash
git clone git@github.com:llvm/llvm-project.git
cd llvm-project
git checkout llvmorg-16.0.0
cmake -S llvm -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
ninja -j 80 -C build
```

### Build Z3 4.12.1

```bash
git clone git@github.com:Z3Prover/z3.git
cd z3
git checkout z3-4.12.1
python3 scripts/mk_make.py
cd build
make -j 80 && sudo make install
```

### Build This Project

1. Update dependency paths in `CMakeLists.txt`:

   ```cmake
   set(LLVM_PATH "/path/to/llvm-project")   # LLVM source and build directory
   set(Z3_PATH   "/path/to/z3")             # Z3 source directory
   ```

2. Build:

   ```bash
   mkdir build && cd build
   cmake ..
   make -j 32
   ```

After building, the following five executables will be available in `build/`:

| Executable  | Description                              |
|-------------|------------------------------------------|
| `slot`      | Full pipeline: SMT → LLVM opt → SMT      |
| `fastslot`  | Fast pipeline: pass-file-driven          |
| `smt2llvm`  | Single step: SMT → LLVM IR              |
| `llvm2smt`  | Single step: LLVM IR → SMT              |
| `llvm2feat` | Feature extraction from LLVM IR         |

## Usage

### `slot` — Full SMT Optimization Pipeline

```
Usage: slot [options]

Required:
  -s <file>          Input SMT-LIB 2 file

Optional:
  -o <file>          Output optimized SMT-LIB 2 file (default: stdout)
  -lu <file>         Output pre-optimization LLVM IR
  -lo <file>         Output post-optimization LLVM IR
  -t <file>          Output timing statistics (CSV format)
  -m                 Convert constant right-shift operations to multiplications
  -p <file>          Read pass list from file
  -h                 Show help
```

```bash
./slot -m \
  -s problem.smt2 \
  -lu before.ll -lo after.ll \
  -o result.smt2 \
  -p ../passes-run.txt
```

### `fastslot` — Fast Pipeline

Similar to `slot` but only supports pass-file-driven optimization via `-p`. Suitable for batch scripting.

```bash
./fastslot -m -s input.smt2 -o result.smt2 -p ../passes-run.txt
```

### `smt2llvm` / `llvm2smt` — Single-Step Conversion

```bash
./smt2llvm -s input.smt2 -lu output.ll
./llvm2smt -lo optimized.ll -o result.smt2
```

### `llvm2feat` — Feature Extraction

Extracts 58-dimensional program features (instruction counts, control flow, constant statistics, etc.) from LLVM IR.

```bash
./smt2llvm -s input.smt2 -lu input.ll
./llvm2feat -lo input.ll -f ./features/
```

## Common Workflows

### End-to-End SMT Simplification

```bash
./slot -m -s problem.smt2 -o simplified.smt2 -p ../passes-run.txt
```

### Step-by-Step with Intermediate IR Inspection

```bash
# Step 1: SMT → LLVM IR
./smt2llvm -s problem.smt2 -lu before.ll

# Step 2: Manually optimize with opt (optional, useful for debugging)
opt -passes="instcombine,gvn" -S before.ll -o after.ll

# Step 3: LLVM IR → SMT
./llvm2smt -lo after.ll -o simplified.smt2
```

### Batch Feature Extraction

```bash
for f in corpus/*.smt2; do
  base=$(basename "$f" .smt2)
  ./smt2llvm -s "$f" -lu /tmp/${base}.ll
  ./llvm2feat -lo /tmp/${base}.ll -f features/${base}/
done
```

---

## Pass Configuration Files

| File                    | Count | Description                                  |
|-------------------------|-------|----------------------------------------------|
| `passes-slot-old.txt`   | 9     | Core passes used by the original SLOT        |
| `passes-run.txt`        | 41    | All currently supported meaningful passes    |
| `passes-filter.txt`     | 25    | Subset with clear optimization benefit       |
| `passes-useful.txt`     | 33    | Optimization-related passes (incl. loop/vec) |
| `passes-16.txt`         | 80    | Base pass list available in LLVM 16          |
| `passes-all-llvm16.txt` | 353   | Full LLVM 16 pass list (incl. experimental)  |

Pass names follow the LLVM new Pass Manager (`-passes=` syntax). See `FunctionPasses.md` for detailed descriptions.
