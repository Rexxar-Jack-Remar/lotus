# GA Optimizer

Automatically searches for the optimal LLVM pass combination for SMT constraint solving using a genetic algorithm, to reduce SMT solve time.

## Core Idea

For each SMT constraint file, the full optimization pipeline is executed and its total time is used as the GA fitness function:

```
SMT → smt2llvm → LLVM IR → opt (pass combination) → LLVM IR → llvm2smt → SMT → solver
```

The GA searches over the boolean pass-enable space to find the pass set that minimizes total time, and writes results to a CSV for further analysis.

## Directory Structure

```
GA/
├── config.yaml              # Global path and parameter configuration
├── ga_optimizer.py          # Main entry: GA optimizer
├── phrase_time_counter.py   # Base class for per-stage timing
├── delta_debugger.py        # Post-processing: prune redundant passes
├── mlopt/
│   ├── ga_opt.py            # GA algorithm core (population, selection, crossover)
│   └── params.py            # Parameter encoding (Params class, mutation, crossover)
└── utils/                   # Data preprocessing scripts (see utils/README.md)
```

## Configuration (`config.yaml`)

Confirm the following fields before running any script:

| Field | Description | Example |
|-------|-------------|---------|
| `slot_dir` | SLOT binary directory; must contain `fastslot`, `slot`, `smt2llvm`, `llvm2smt` | `/path/to/build/` |
| `llvm_dir` | LLVM binary directory; must contain `opt` | `/path/to/llvm-project/build/bin` |
| `dataset_dir` | Root directory of the SMTLIB dataset | `/data/QF_BV` |
| `smt_solver_path` | SMT solver executable; supports Z3 / CVC5 / Boolector | `/usr/bin/z3` |
| `output_csv_path` | Output path for GA results CSV | `/path/to/RQ1.csv` |
| `smac_log_output_directory` | SMAC log output directory | `/path/to/SMAC` |

## Workflow

### Step 1: Data Preprocessing

See [utils/README.md](utils/README.md) for details. Produces per-solver SMT file lists segmented by solve time.

### Step 2: Run GA Optimization

```bash
python -m ga_optimizer
```

`GAOptimizer` (extends `PhraseTimeCounter`) processes each SMT file in `fast_smt_path`:

1. Converts SMT to LLVM IR (`smt2llvm`)
2. Uses the SLOT default pass combination as the baseline
3. Runs GA (32 iterations, population size 32) to search for the optimal pass combination
4. Writes all pass combinations that beat the baseline to `output_csv_path`

Output CSV format: `file_relative_path, cost_time, <pass1>, <pass2>, ..., status`

Runs with 40 parallel processes by default.

### Step 3: Prune Redundant Passes (optional)

```bash
python -m delta_debugger
```

Reads the GA label CSV, takes the best pass combination per file, and uses iterative deletion to find the minimal pass subset that produces identical LLVM IR output. Results written to `Refine_<original_filename>.csv`.

Update `base_name` and path variables in `delta_debugger.py` before running.

### Step 4: Per-Stage Timing Analysis (optional)

```bash
python -m phrase_time_counter
```

Reads the GA label CSV and compares total time across four strategies:

| Strategy | Description |
|----------|-------------|
| `default_solve_time` | Solve original SMT directly |
| `slot_solve_time` | Solve after SLOT default pass optimization |
| `ga_best_solve_time` | Best pass combination found by GA |
| `ga_worst_solve_time` | Worst pass combination found by GA |
| `o3_solve_time` | Solve after `-O3` optimization |

Results written to the solver-specific `*_ga_cost_time_path` in `config.py`.

## Module Reference

### `mlopt/ga_opt.py` — GA Algorithm

| Parameter | Value | Description |
|-----------|-------|-------------|
| `_population_size` | 32 | Population size |
| `_retain_percentage` | 0.25 | Elite retention ratio |

Each iteration: `evaluate` (compute fitness) → `repopulate` (selection + crossover + mutation). Fitness is total pipeline time; `4294967295` (`OPT_INF`) indicates timeout or failure.

### `mlopt/params.py` — Parameter Encoding

Each LLVM pass is encoded as a boolean parameter (`true`/`false`). The `Params` class supports:
- `load(optlist)`: initialize from a list of strings
- `mutate()`: randomly flip each parameter with 0.5 probability
- `crossover(p1, p2)`: single-point crossover
- `to_cmd_args()`: convert enabled passes to a command-line argument list

### `phrase_time_counter.py` — Pipeline Timing Base Class

`PhraseTimeCounter` wraps all pipeline stage calls:

| Method | Description |
|--------|-------------|
| `smt_to_llvm()` | SMT → LLVM IR; returns conversion time |
| `try_optimize(path, passes)` | Run `opt` with given passes; returns time |
| `try_optimize_with_o3(path)` | Run `opt` with `-O3`; returns time |
| `llvm_to_smt(llvm, smt)` | LLVM IR → SMT; returns conversion time |
| `smt_solve_time(smt)` | Run solver; returns solve time |
| `get_all_phrase_time(use_o3, passes)` | Returns total pipeline time |

Intermediate files are written to `/tmp/rq1/`; timeouts return `OPT_INF`.

## Logs

Runtime logs written to `~/logs/RQ1/`, rotated daily, retained for 7 days:

- `rq1.info`: Normal flow logs with per-stage timing
- `rq1.error`: Timeouts, conversion failures, solver result mismatches
