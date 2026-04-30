# TUNA
Code repository for [Compiler Optimization-Based SMT Simplifications: An In-Depth Study](https://dl.acm.org/doi/10.1145/3795879).


NOTE: this dir is for LLVM 16 (not LLVM 14). Sot is not built by default.

TUNA speeds up SMT solving by converting SMT formulas to LLVM IR, applying compiler optimizations, and converting back. It includes a genetic algorithm to automatically find the best combination of LLVM passes for a given dataset.

## Repository Structure

```
TUNA/
├── TUNA-Opt/          # Core optimization framework
│   ├── SMT2LLVM/      # Bidirectional SMT ↔ LLVM IR conversion tools
│   └── GA/            # Genetic algorithm for pass selection
└── TUNA-Learn/        # (coming soon) Machine learning-based optimization framework
```

## Dependencies

- LLVM 16.0.0
- Z3 4.12.1
- CMake + Ninja
- Python 3

## Build

```bash
cd TUNA-Opt/SMT2LLVM
mkdir build && cd build
cmake ..
make -j 32
```

This produces five binaries: `slot`, `fastslot`, `smt2llvm`, `llvm2smt`, `llvm2feat`.

## Usage

Simplify an SMT file using a set of LLVM passes:

```bash
./slot -m -s problem.smt2 -o simplified.smt2 -p passes.txt
```

Run the genetic algorithm to find optimal pass combinations for a dataset:

```bash
# Edit TUNA-Opt/GA/config.yaml with your paths first
cd TUNA-Opt/GA
python -m ga_optimizer
```

## How It Works

1. SMT formula is translated to LLVM IR
2. LLVM optimization passes are applied
3. Optimized IR is translated back to SMT
4. The simplified formula is solved — typically faster than the original

The GA searches over combinations of 41 supported passes to minimize total solve time across a benchmark dataset.

## Citation

```bibtex
@article{tuna2025,
  title     = {Compiler Optimization-Based SMT Simplifications: An In-Depth Study},
  booktitle = {ACM},
  url       = {https://dl.acm.org/doi/10.1145/3795879}
}
```

## 