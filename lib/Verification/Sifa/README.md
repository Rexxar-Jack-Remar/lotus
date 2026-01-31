# Sifa (Verification/Sifa)

C++/LLVM port of **Ultimate Library-Sifa** (Symbolic Interpretation with Fluid Abstractions) for lotus, aligned with `ultimate-0.3.1/trunk/source/Library-Sifa`.

## Bitcode Support (C/C++ roadmap)

The primary “real LLVM IR” entry point is `lotus::sifa::analyzeSymAbs*()` (see `include/Verification/Sifa/SifaSymAbs.h`), which runs Sifa with a SymbolicAbstraction-backed abstract domain.

### Supported subset (strict mode)

By default, `SifaSymAbsOptions::validateLlvmSubset` is enabled. The current *well-defined* supported subset is:

- LLVM IR compatible with lotus’ LLVM build (SymbolicAbstraction currently targets LLVM 14).
- Scalar integers (`i1`…`i64`) and pointers.
- Control-flow: `br`, `switch`, `phi`, `select`, `ret`.
- Scalar ops: integer arith/bitwise (`add/sub/mul/div/rem/shifts/and/or/xor`), casts (`zext/sext/trunc`, `ptrtoint/inttoptr/bitcast`), `icmp`.
- Memory operations are allowed structurally (`alloca`, `load`, `store`, `getelementptr`), but precision depends on the chosen abstract domain (see below).

Unsupported in the strict subset (will raise `std::invalid_argument`):

- Vectors and vector instructions.
- First-class aggregate values (e.g. struct-typed SSA values / `extractvalue` / `insertvalue`).
- Exceptions/EH (`invoke`, landingpads, funclets, `resume`, …).
- Atomics/fences.
- Varargs (`va_arg`).
- `float` (and `fptrunc`/`fpext`). `double` is optional via `SifaSymAbsOptions::allowDouble`.

### Practical C/C++ guidance

For a usable “C/C++ bitcode” workflow today, a good starting point is:

- Compile without exceptions and atomics if you want strict validation to pass.
- Prefer IR that is close to SSA for scalar locals (e.g. compile with optimizations or run `mem2reg`), otherwise most domains will treat loads/stores conservatively.

### Domains and precision (memory)

The domain string (`SifaSymAbsOptions::abstractDomain`) controls what information is tracked precisely. For programs that use memory heavily (typical unoptimized C/C++), consider including a memory-aware domain (e.g. `MemRange` / `ValidRegion`) instead of only numeric domains like `Interval, Octagon`.

### Roadmap

Key next steps to broaden “C/C++ bitcode” coverage:

- `float` support (and `fptrunc`/`fpext`) in `FloatingPointModel`.
- First-class aggregates / struct-typed SSA values (or a stricter “memory-only aggregates” discipline + checks).
- Exceptions/EH and atomics if needed for target workloads.
