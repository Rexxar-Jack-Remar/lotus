# Program Verification

Lotus provides verification infrastructure, preprocessing passes, frontends,
and multiple verification backends based on abstract interpretation and
symbolic reasoning.

| Subdir | Purpose |
|--------|---------|
| **Analysis** | Pre-verification module analyses such as module checking, instruction and loop classification, instruction counting, and test-target extraction. |
| **Backend** | Common backend interface and backend runners for invoking verification engines and normalizing results. |
| **Frontend** | Predicate/Boolean-program parsing and lowering infrastructure used to build verification-oriented IR. |
| **Sifa** | Symbolic Interpretation with Fluid Abstractions: interprocedural symbolic interpretation over ICFG-style procedure graphs and regex-DAG summaries. See `Sifa/README.md`. |
| **SymAbsAI** | SMT-backed abstract interpretation framework with reusable transfer semantics, fixpoint engines, and abstract domains. See `SymAbsAI/README.md`. |
| **Transform** | IR transformations for verification, including CFG normalization, loop/control-flow rewriting, memory instrumentation, and nondeterminism injection. |
| **clam** | CLAM-based abstract interpretation backend and supporting SeaDsa/Crab integration code. See `clam/README.md`. |
| **seahorn** | Seahorn integration and support code for Horn-clause-based verification. |
| **smack** | SMACK integration for translating LLVM IR into Boogie-based verification workflows. |

## Analysis

Pre-verification analysis passes:

- [`Analysis/`](Analysis/) contains module analysis utilities.
- `CheckModule.cpp` verifies module integrity.
- `ClassifyInstructions.cpp` classifies instructions for downstream verification workflows.
- `ClassifyLoops.cpp` performs loop analysis.
- `CountInstr.cpp` counts instructions.
- `GetTestTargets.cpp` extracts candidate verification targets.

## Frontend And Backend

- [`Frontend/`](Frontend/) contains Boolean/predicate-program parsing and lowering.
- [`Backend/`](Backend/) provides shared backend execution logic and result parsing.

## Verification Backends

- **CLAM**: abstract interpretation with numerical domains and SeaDsa-based heap abstraction.
- **Sifa**: symbolic interpretation with fluid abstractions, using SymAbsAI-style transfer functions.
- **SymAbsAI**: reusable abstract interpretation framework with SMT-based symbolic abstraction.
- **Seahorn**: Horn-clause-based verification.
- **smack**: translation from LLVM IR to Boogie-based verification workflows.

## Failure-Directed Trimming

[`Transform/FailureDirectedTrimming/`](Transform/FailureDirectedTrimming/)
implements program trimming in the style of Ferles et al. (ESEC/FSE 2017),
including equi-safe reduction, safety-condition inference, and instrumentation
for path pruning.

## Dependencies

- LLVM 14
- Z3
- Boost (for CLAM/CRAB-related components)
- SeaDsa

## References

- CLAM: https://github.com/seahorn/clam
- CRAB: https://github.com/seahorn/crab
- SeaDsa: https://github.com/seahorn/sea-dsa
- Seahorn: https://github.com/seahorn/seahorn
- smack: https://github.com/smackers/smack
