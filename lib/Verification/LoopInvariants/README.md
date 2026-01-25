# LoopInvariants: Loop and Function Invariant Inference

LoopInvariants is an analysis pass that infers loop and function invariants using a **guess-and-check approach** based on ScalarEvolution (SCEV) analysis and SMT solving. The implementation is inspired by xgill's loop invariant inference but adapted to work with LLVM IR and ScalarEvolution.

## Key Features

- **Loop invariants**: Infers properties that hold throughout loop execution
- **Function invariants**: Infers properties about function return values
- **SCEV-based**: Leverages LLVM's ScalarEvolution for pattern recognition
- **SMT proving**: Uses Z3 SMT solver to verify candidate invariants
- **Multiple invariant types**: Monotonicity, bounds, linear relations, assignments, terminators

## Architecture

```
Function → Loop Discovery → Candidate Generation → SMT Proving → Invariant Sets
```

The analysis follows a two-phase approach:

1. **Candidate Generation**: Analyzes loops/functions to generate potential invariants
   - Induction variable analysis
   - Loop bound extraction
   - Value delta analysis
   - Assignment and comparison collection

2. **SMT Proving**: Verifies candidates using Z3
   - Base case: Invariant holds at loop entry
   - Step case: If invariant holds at iteration i, it holds at iteration i+1

## Directory Structure

- **`LoopInvariantAnalysis.cpp`**: Main loop invariant analysis pass
- **`InvariantCandidateGenerator.cpp`**: Generates loop invariant candidates
  - Analyzes induction variables, GEP instructions, loop bounds
  - Generates monotonicity, bound, linear relation, and assignment-based invariants
- **`InvariantProver.cpp`**: Proves loop invariant candidates using Z3
- **`FunctionInvariantAnalysis.cpp`**: Function-level invariant analysis
- **`FunctionInvariantCandidateGenerator.cpp`**: Generates function invariant candidates
- **`FunctionInvariantProver.cpp`**: Proves function invariant candidates

## Invariant Types

### Loop Invariants

- **Monotonic**: `x >= x_initial` or `x <= x_initial` (increasing/decreasing)
- **Bound**: `x < n` or `x <= n` from loop exit conditions
- **Linear Relation**: `(x - x0) * dy == (y - y0) * dx` between related variables
- **Assignment-based**: Properties derived from assignments within the loop
- **Terminator-based**: Properties from loop exit conditions

### Function Invariants

- **Return Bound**: Bounds on function return values
- **Return Non-Negative**: Return value is non-negative
- **Return Comparison**: Relationships between return value and parameters

## Usage

The analysis is registered as an LLVM FunctionAnalysis pass:

```cpp
#include "Verification/LoopInvariants/LoopInvariantAnalysis.h"

// In a pass manager
FAM.registerPass([&] { return LoopInvariantAnalysis(); });

// Query results
auto &LIResult = FAM.getResult<LoopInvariantAnalysis>(F);
if (auto *InvSet = LIResult.getInvariants(Loop)) {
  // Use invariants
}
```

### Printer Pass

```bash
# Print loop invariants
opt -passes="function(loop-invariant-printer)" <input.ll>
```

## Dependencies

- **LLVM Analyses**: LoopInfo, ScalarEvolution, DominatorTree
- **SMT Solver**: Z3 (via `Solvers/SMT/LIBSMT/Z3Expr.h`)

## See Also

- Header files: `include/Verification/LoopInvariants/`
- Tests: `tests/unit/LoopInvariants/`
