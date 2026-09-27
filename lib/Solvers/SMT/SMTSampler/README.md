# SMT Samplers


This folder contains multiple samplers for extracting satisfying models from SMT
formulas, each optimized for a different strategy.

## QuickSampler
- Input: CNF (DIMACS) with optional `c ind` lines to mark independent variables.
- Output: `<input>.samples`, one line per sample in the format `<mutations>: <bitstring>`.
- Notes: Uses mutation-based exploration; stops on time or sample limits.

## IntervalSampler
- Input: SMT-LIB bit-vector formulas (single file or directory).
- Output: `res.log` (append-only summary stats).
- Notes: Computes per-variable bounds with optimize, then samples uniformly.

## RegionSampler
- Input: SMT-LIB bit-vector formulas.
- Output: `<input>.abs.samples`, first line is a header of variable names.
- Notes: Builds a linear abstraction (Zone or Octagon) and uses a walk (default
  hit-and-run) to propose samples, then validates against the original formula.

## Related Work

- FMCAD '19: "GUIDEDSAMPLER: Coverage-guided Sampling of SMT Solutions",
  Rafael Dutra, Jonathan Bachrach, Koushik Sen.
  https://github.com/RafaelTupynamba/GuidedSampler
- ICSE '18: "Efficient Sampling of SAT Solutions for Testing".
  https://github.com/RafaelTupynamba/quicksampler (implemented here as
  `QuickSampler.cpp`)
