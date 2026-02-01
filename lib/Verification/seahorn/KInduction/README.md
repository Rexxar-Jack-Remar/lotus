# K-Induction Engine

k-induction verification that reuses Seahorn PathBMC.

## Design

- **Base case (per k)**: Run PathBMC on the program with all loops peeled **k** times. If any path reaches an error → **BUG**.
- **Step case (per k)**: Run PathBMC on the program with all loops peeled **k+1** times. If no path reaches an error (UNSAT) → **SAFE** (property holds for all executions within k+1 steps; by increasing k we effectively prove safety when step case succeeds).
- **Algorithm**: For k = 1, 2, … (until timeout or cap):
  1. Clone the module and peel loops **(k+1)** times.
  2. Run PathBMC on the peeled clone.
  3. If SAT → return **BUG** (counterexample in ≤ k+1 steps).
  4. If UNSAT → return **SAFE** (no error in ≤ k+1 steps; sound proof).

This is **incremental bounded model checking**: we increase the bound until PathBMC reports UNSAT. It reuses PathBMC’s path enumeration, SMT/Crab path solving, and blocking clauses without modifying PathBMC.

Optional future extension: true k-induction step (assume safe for k steps, encode only step k+1) for smaller SMT queries.

## Dependencies

- Seahorn PathBMC (PathBmcEngine), CutPointGraph, OperationalSemantics (BvOpSem).
- Loop peeler: `seahorn::createLoopPeelerPass(unsigned Num)`.
- Requires CLAM for full PathBMC (path solving with Crab); falls back to encode-only if unavailable.

## Options

- `k-min`, `k-max`: range of k to try (default 1..∞ until timeout).
- `timeout`: total CPU timeout for the engine.
- `entry`: entry function (default `main`).

## Usage

- As an LLVM pass: run after CutPointGraph, ShadowMem, etc. (same as BmcPass). Use `createKInductionPass()` and add to the pass pipeline before or instead of PathBMC.
- From the seahorn tool: e.g. `--run=kinduction` or a dedicated `kinduction` frontend that builds the same pipeline as seahorn but runs KInduction instead of PathBMC.

## Files

- `KInductionEngine.hh` / `KInductionEngine.cc`: orchestration (clone, peel, run PathBMC, interpret result).
- `KInductionPass.cc`: LLVM ModulePass that runs the engine on the entry function.
