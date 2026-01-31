# Sifa (Verification/Sifa)

C++/LLVM port of **Ultimate Library-Sifa** (Symbolic Interpretation with Fluid Abstractions) for lotus, aligned with `ultimate-0.3.1/trunk/source/Library-Sifa`.

## Abstract domains (include/Verification/Sifa/Domain/)

Domain implementations are Ultimate-aligned and follow the same roles as in Ultimate's Library-Sifa and Sifa plugin.

### Domain choice (convenience APIs)

The user chooses the domain by calling the corresponding API or by switching on **SifaOptions::domainKind** in their code:

| Domain | API | State type |
|--------|-----|------------|
| **Reachability** | `isReachable()`, `isReachableInterprocedural()` | `bool` |
| **Interval** | `analyzeToWithIntervalDomain()` | `IntervalState` |
| **Octagon** | `analyzeToWithOctagonDomain()` | `OctagonState` |
| **Eq** | `analyzeToWithEqDomain()` | `EqState` |
| **ExplicitValue** | `analyzeToWithExplicitValueDomain()` | `ExplicitValueState` |

Example: choose at run time using `SifaOptions::domainKind`:

```cpp
switch (options.domainKind) {
  case SifaDomainKind::Reachability:
    return isReachable(F, target, options);
  case SifaDomainKind::Interval:
    return analyzeToWithIntervalDomain(F, target, IntervalState(false), options);
  case SifaDomainKind::Octagon:
    return analyzeToWithOctagonDomain(F, target, OctagonState(false), options);
  // ...
}
```

### Other domain types

- **CompoundDomain**, **StatsWrapperDomain** – Use the template `analyzeTo<StateT>(..., domain, options)` with a constructed domain.
- **StateBasedDomain**, **RelationCheckUtil**, **TermToInterval**, **DnfToExplicitValue** – Supporting types used by the above domains.

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

### Per-block transfer strategy (precision-performance trade-off)

You can choose **instruction-by-instruction** (more precise, slower) or **block-wise** (fast havoc, less precise) per basic block via **SifaOptions::blockTransferPolicy**:

- **BlockTransferPolicy**: set of blocks (or a predicate) that use block-wise transfer; all others use instruction-by-instruction.
- **Instruction-wise** (default): `applyBlockTransfer(bb, in)` — full semantics per instruction.
- **Block-wise**: `applyBlockWiseHavoc(bb, in)` — treat block as black box (havoc defined values); sound but less precise.

Example: use block-wise for hot or large blocks to speed up analysis.

```cpp
BlockTransferPolicy policy;
policy.addBlockWise(&someBasicBlock);
SifaOptions options;
options.blockTransferPolicy = policy;
auto state = analyzeToWithIntervalDomain(F, target, IntervalState(false), options);
```

### Instruction-level block transfer (soundness)

The value domains (Interval, Eq, ExplicitValue) apply **real instruction-level transfer** on CFG edges so that `post(Edge)` models program semantics (sound over-approximation):

- **IntervalDomain**: Full block transfer in `lib/Verification/Sifa/Domain/IntervalDomain.cpp` — binary ops (add/sub/mul/div/rem/shifts/and/or/xor), casts (trunc/zext/sext), icmp, select, phi; load/store/call/gep/alloca yield top.
- **EqDomain**: Copy/phi/select equality propagation (unite result with operands); other instructions ensure the result is in the state.
- **ExplicitValueDomain**: Constant propagation over instructions (constants, arithmetic, casts, phi, select).
- **OctagonDomain**: Block transfer in `lib/Verification/Sifa/Domain/OctagonDomain.cpp` — copy/constant/affine (res = src, res = c, res = src + k); phi/select/non-linear ops havoc the result.

### Domains and precision (memory)

The domain string (`SifaSymAbsOptions::abstractDomain`) controls what information is tracked precisely. For programs that use memory heavily (typical unoptimized C/C++), consider including a memory-aware domain (e.g. `MemRange` / `ValidRegion`) instead of only numeric domains like `Interval, Octagon`.

### Roadmap

Key next steps to broaden “C/C++ bitcode” coverage:

- `float` support (and `fptrunc`/`fpext`) in `FloatingPointModel`.
- First-class aggregates / struct-typed SSA values (or a stricter “memory-only aggregates” discipline + checks).
- Exceptions/EH and atomics if needed for target workloads.
