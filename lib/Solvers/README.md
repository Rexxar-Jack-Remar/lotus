# Solvers

SMT and BDD solver integrations for Lotus.

## Generic Solver Infrastructure

- **EGraph** - Solver-agnostic equality graph and rewrite engine inspired by
  `egg`

## SMT

SMT solver wrappers and utilities:

- **LIBSMT** – Z3 API wrapper with factory pattern
- **QuantSimp** – Quantifier simplification using E-graphs
- **SLOT** – LLVM IR to SMT formula translation
- **SMTSampler** – SMT model sampling
- **STAUB** – STAUB solver integration
- **SymAbs** – SMT formula abstraction (bit-vector to linear integer)
- **TUNA** – Accelerates SMT solving via SMT↔LLVM optimization, with GA-based LLVM pass selection

## References

- Z3: https://github.com/Z3Prover/z3
