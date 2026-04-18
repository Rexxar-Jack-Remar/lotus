# README

A collection of relatively standalone data preprocessing and utility scripts for preparing training data for the GA optimizer.

## Prerequisites (`config.yaml`)

All scripts depend on `config.yaml` in the project root. **Confirm the following fields before running any script:**

| Field | Description | Example |
|-------|-------------|---------|
| `slot_dir` | SLOT binary directory; must contain `fastslot`, `slot`, `smt2llvm`, `llvm2smt` | `/path/to/build/` |
| `llvm_dir` | LLVM binary directory; must contain `opt` | `/path/to/llvm-project/build/bin` |
| `dataset_dir` | Root directory of the SMTLIB dataset (`.smt2` files) | `/data/QF_BV` |
| `smt_solver_path` | SMT solver executable; supports Z3 / CVC5 / Boolector | `/usr/bin/z3` |
| `output_csv_path` | Output path for RQ1 results CSV | `/path/to/RQ1.csv` |
| `smac_log_output_directory` | SMAC optimization log output directory | `/path/to/SMAC` |

Per-script config dependencies:

- **`slot_validate.py`**: requires `slot_dir` (uses `fastslot`), `dataset_dir`
- **`z3_validate.py`**: requires `smt_solver_path`, `dataset_dir` (solver determines which category file to write)
- **`find_under_1s.py`**: no extra config; depends on files produced by the previous two steps
- **`add_smt_status_to_ga_label.py`**: requires `dataset_dir`
- **`refine_slot_data.py`**: requires `slot_dir` (uses original `slot`), `dataset_dir`

> `smt_solver_path` determines which category file `z3_validate.py` writes to. When switching solvers, also update the `TODO` comments in the script to point the output path to the corresponding solver category path (e.g. `CONFIG.cvc5_600s_smt_path`).

## Preprocessing Pipeline

The overall pipeline consists of the following stages:

### Stage 1: Filter Translatable SMT Constraints

**`slot_validate.py`**

Iterates over all `.smt2` files in the dataset and uses the SLOT tool (`fastslot`) to attempt translation on each. Files that complete within the timeout (default 20s) are written to `fast_smt_path` (`fastslot_under_20s_smt.txt`).

```bash
python -m utils.slot_validate
```

### Stage 2: Segment by SMT Solve Time

**`z3_validate.py`**

Runs the specified SMT solver (Z3 / CVC5 / Boolector) on the files from Stage 1 and segments them by solve time (1s, 30s, 60s, 120s, 300s, 600s), writing each segment to the corresponding category file.

> Note: update the `TODO` comments in the script to specify the input file and output file paths (matching the category paths in `config.py`) before running.

```bash
python -m utils.z3_validate
```

**`find_under_1s.py`**

Subtracts the `over_1s` list from `fast_smt_path` to produce the list of SMT files with solve time under 1s, written to the `under_1s` path.

```bash
python -m utils.find_under_1s
```

## Utility Modules

| File | Description |
|------|-------------|
| `config.py` | Global config; reads `config.yaml` and centralizes all paths and parameters |
| `file_utils.py` | File utilities: enumerate SMT files, read SMT lists, initialize CSVs, etc. |
| `logger.py` | Logging utility; outputs to `~/logs/RQ1/`, supports TraceID (via `asgi_correlation_id`) |

## Category Path Convention (`config.py`)

Solver category files are stored under `resources/<smt_category>/<solver>_category/`, named as:

```
<solver>_over_<low>s_under_<high>s_smt.txt
```

For example: `z3_over_1s_under_30s_smt.txt` contains the list of constraints with Z3 solve time between 1s and 30s.
