# TUNA-Opt

TUNA-Opt is an automatic optimization framework for SMT constraint solving. It contains two main modules:

- **SMT2LLVM**: A bidirectional conversion toolkit between SMT-LIB 2 and LLVM IR
- **GA**: A genetic algorithm-based optimizer for automatic LLVM pass combination search

Core idea: translate SMT formulas to LLVM IR, simplify them using LLVM optimization passes, then convert back to SMT-LIB 2 to reduce solver time.

## Directory Structure

```
TUNA-Opt/
├── SMT2LLVM/                # SMT ↔ LLVM IR conversion toolkit
│   ├── include/
│   ├── src/
│   │   └── tools/           # slot / fastslot / smt2llvm / llvm2smt / llvm2feat
│   ├── passes-run.txt        # 41 meaningful passes
│   ├── passes-filter.txt     # 25-pass optimization subset
│   └── FunctionPasses.md
└── GA/                      # Genetic algorithm optimizer
    ├── config.yaml
    ├── ga_optimizer.py
    ├── delta_debugger.py
    ├── phrase_time_counter.py
    ├── mlopt/
    │   ├── ga_opt.py
    │   └── params.py
    └── utils/               # Data preprocessing scripts
```

---

## SMT2LLVM

A bidirectional conversion toolkit extended from [SLOT](https://github.com/TUNA-SMT/SLOT). Builds five executables:

| Executable  | Description                              |
|-------------|------------------------------------------|
| `slot`      | Full pipeline: SMT → LLVM opt → SMT      |
| `fastslot`  | Fast pipeline: pass-file-driven          |
| `smt2llvm`  | Single step: SMT → LLVM IR              |
| `llvm2smt`  | Single step: LLVM IR → SMT              |
| `llvm2feat` | Feature extraction from LLVM IR         |

### Installation

**System dependencies**

```bash
sudo apt install -y git gcc g++ cmake ninja-build python3 \
                    zlib1g-dev libtinfo-dev libxml2-dev
```

**Build LLVM 16.0.0**

```bash
git clone git@github.com:llvm/llvm-project.git
cd llvm-project
git checkout llvmorg-16.0.0
cmake -S llvm -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
ninja -j 80 -C build
```

**Build Z3 4.12.1**

```bash
git clone git@github.com:Z3Prover/z3.git
cd z3
git checkout z3-4.12.1
python3 scripts/mk_make.py
cd build
make -j 80 && sudo make install
```

**Build this project**

1. Update dependency paths in `SMT2LLVM/CMakeLists.txt`:

   ```cmake
   set(LLVM_PATH "/path/to/llvm-project")
   set(Z3_PATH   "/path/to/z3")
   ```

2. Build:

   ```bash
   cd SMT2LLVM
   mkdir build && cd build
   cmake ..
   make -j 32
   ```

### Usage

**`slot` — full SMT optimization pipeline**

```bash
./slot -m \
  -s problem.smt2 \
  -lu before.ll -lo after.ll \
  -o result.smt2 \
  -p ../passes-run.txt
```

**`fastslot` — fast pipeline for batch processing**

```bash
./fastslot -m -s input.smt2 -o result.smt2 -p ../passes-run.txt
```

**`smt2llvm` / `llvm2smt` — single-step conversion**

```bash
./smt2llvm -s input.smt2 -lu output.ll
./llvm2smt -lo optimized.ll -o result.smt2
```

**`llvm2feat` — feature extraction**

Extracts 58-dimensional program features (instruction counts, control flow, constant statistics, etc.) from LLVM IR.

```bash
./smt2llvm -s input.smt2 -lu input.ll
./llvm2feat -lo input.ll -f ./features/
```

### Common Workflows

**End-to-end SMT simplification**

```bash
./slot -m -s problem.smt2 -o simplified.smt2 -p ../passes-run.txt
```

**Step-by-step with intermediate IR inspection**

```bash
./smt2llvm -s problem.smt2 -lu before.ll
opt -passes="instcombine,gvn" -S before.ll -o after.ll
./llvm2smt -lo after.ll -o simplified.smt2
```

**Batch feature extraction**

```bash
for f in corpus/*.smt2; do
  base=$(basename "$f" .smt2)
  ./smt2llvm -s "$f" -lu /tmp/${base}.ll
  ./llvm2feat -lo /tmp/${base}.ll -f features/${base}/
done
```

### Pass Configuration Files

| File                    | Count | Description                                  |
|-------------------------|-------|----------------------------------------------|
| `passes-slot-old.txt`   | 9     | Core passes used by the original SLOT        |
| `passes-run.txt`        | 41    | All currently supported meaningful passes    |
| `passes-filter.txt`     | 25    | Subset with clear optimization benefit       |
| `passes-useful.txt`     | 33    | Optimization-related passes (incl. loop/vec) |
| `passes-16.txt`         | 80    | Base pass list available in LLVM 16          |
| `passes-all-llvm16.txt` | 353   | Full LLVM 16 pass list (incl. experimental)  |

---

## GA Optimizer

Automatically searches for the optimal LLVM pass combination for SMT constraints using a genetic algorithm to minimize solve time.

Full optimization pipeline:

```
SMT → smt2llvm → LLVM IR → opt (pass combination) → LLVM IR → llvm2smt → SMT → solver
```

The GA searches over the boolean pass-enable space, using total pipeline time as the fitness function.

### Configuration (`GA/config.yaml`)

| Field | Description | Example |
|-------|-------------|---------|
| `slot_dir` | Directory containing SLOT binaries (`fastslot`, `slot`, `smt2llvm`, `llvm2smt`) | `/path/to/build/` |
| `llvm_dir` | LLVM binary directory containing `opt` | `/path/to/llvm-project/build/bin` |
| `dataset_dir` | Root directory of the SMTLIB dataset (`.smt2` files) | `/data/QF_BV` |
| `smt_solver_path` | SMT solver executable; supports Z3 / CVC5 / Boolector | `/usr/bin/z3` |
| `output_csv_path` | Output path for GA results CSV | `/path/to/RQ1.csv` |
| `smac_log_output_directory` | SMAC log output directory | `/path/to/SMAC` |

### Workflow

**Step 1: Data preprocessing**

See [GA/utils/README.md](GA/utils/README.md) for details. Produces per-solver SMT file lists segmented by solve time.

```bash
python -m utils.slot_validate
python -m utils.z3_validate
python -m utils.find_under_1s
```

**Step 2: Run GA optimization**

```bash
cd GA
python -m ga_optimizer
```

Runs GA (32 iterations, population size 32, 40 parallel processes) on each SMT file in `fast_smt_path`. Pass combinations that beat the baseline solve time are written to `output_csv_path`.

Output CSV format: `file_relative_path, cost_time, <pass1>, <pass2>, ..., status`

**Step 3: Prune redundant passes (optional)**

```bash
python -m delta_debugger
```

For the best pass combination found by GA, iteratively removes passes to find the minimal subset that produces identical LLVM IR output. Results written to `Refine_<original_filename>.csv`.

**Step 4: Timing analysis (optional)**

```bash
python -m phrase_time_counter
```

Compares total pipeline time across four strategies:

| Strategy | Description |
|----------|-------------|
| `default_solve_time` | Solve original SMT directly |
| `slot_solve_time` | Solve after SLOT default pass optimization |
| `ga_best_solve_time` | Best pass combination found by GA |
| `ga_worst_solve_time` | Worst pass combination found by GA |
| `o3_solve_time` | Solve after `-O3` optimization |

### Module Overview

**`mlopt/ga_opt.py`**: GA core — population size 32, 25% elite retention. Each iteration: `evaluate` (fitness) → `repopulate` (selection + crossover + mutation).

**`mlopt/params.py`**: Each LLVM pass encoded as a boolean parameter. Supports `mutate()` (0.5 flip probability) and `crossover()` (single-point).

**`phrase_time_counter.py`**: Wraps all pipeline stages (`smt_to_llvm`, `try_optimize`, `llvm_to_smt`, `smt_solve_time`). Intermediate files written to `/tmp/rq1/`; timeouts return `OPT_INF`.

### Logs

Runtime logs written to `~/logs/RQ1/`, rotated daily, retained for 7 days:

- `rq1.info`: Normal flow logs with per-stage timing
- `rq1.error`: Timeouts, conversion failures, solver result mismatches
