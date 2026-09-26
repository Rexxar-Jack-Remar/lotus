# Mosaic micro inputs

These are small, concrete 4-bit Horn SMT-LIB2 inputs for parser and solver
smoke testing. They cover all eleven workloads in the Mosaic 1.0 artifact at
one width, but they are not the complete ATVA experimental dataset.

The original artifact parameterizes all eleven workloads by bit-vector width
and evaluates widths 4 through 63. The `-bv4` suffix here makes the concrete
width explicit. These files are not registered by `lotus-verify-mosaic` and
are not installed; pass their paths to the verifier directly.

```bash
build/bin/lotus-verify-mosaic \
  benchmarks/micro/Verification/Mosaic/max-inv-bv4.smt2
```
