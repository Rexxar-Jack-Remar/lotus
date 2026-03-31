# Sparrow Andersen Analysis

## Optimizations

The implementation includes four optional optimizations that can be enabled via command-line flags:

| Optimization | Paper | Option |
|--------------|-------|--------|
| **HVN** | "Exploiting Pointer and Location Equivalence to Optimize Pointer Analysis" (SAS 2007) | `--enable-hvn` |
| **HU** | "Exploiting Pointer and Location Equivalence to Optimize Pointer Analysis" (SAS 2007) | `--enable-hu` |
| **HCD** | "The Ant and the Grasshopper: Fast and Accurate Pointer Analysis for Millions of Lines of Code" (PLDI 2007) | `--enable-hcd` |
| **LCD** | "The Ant and the Grasshopper: Fast and Accurate Pointer Analysis for Millions of Lines of Code" (PLDI 2007) | `--enable-lcd` |

**Descriptions:**
- **HVN** (Hash-based Value Numbering): Performs offline variable substitution without evaluating unions to identify pointer equivalences and merge nodes
- **HU** (HVN with deReference and Union): Performs offline variable substitution including union evaluation to identify pointer and location equivalences
- **HCD** (Hybrid Cycle Detection): Offline cycle detection that identifies collapse targets before constraint solving, then applies them during online solving
- **LCD** (Lazy Cycle Detection): Online cycle detection that batches cycle candidates and checks them together for efficiency

All optimizations are **disabled by default**. HVN and HU run during the constraint optimization phase, while HCD and LCD run during the constraint solving phase.

## Context sensitivity

- `--andersen-k-cs=<k>` selects the call-site sensitivity (`0 <= k <= 32`):
  - `0` (default): context-insensitive
  - `1`: 1-CFA (last call site)
  - `2`: 2-CFA (last two call sites)
  - `k > 2`: k-CFA (last `k` call sites)
- If `k` is greater than 32, SparrowAA emits a warning and falls back to
  context-insensitive analysis (`k=0`).
- Andersen now uses an internal call-string context manager to keep each call-site
  history distinct while still sharing universal/null nodes. This avoids the
  equality bug in the external `KCallSite` helper and ensures points-to facts are
  tracked per context as expected.
