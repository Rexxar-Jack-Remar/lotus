# The Phenix Alias Analsyis Toolkits

This directory contains various alias analysis implementations and toolkits used in the Phenix project.

## Analysis Comparison Table

| Analysis | Tool/Command | Analysis Type | Flow-Sensitive | Context-Sensitive | Field-Sensitive | Third-Party | Notes |
|----------|--------------|---------------|----------------|-------------------|-----------------|-------------|-------|
| **SparrowAA** | `sparrow-aa` | Inclusion-based | ❌ No | ✅ Yes (CI, 1-CFA, 2-CFA, etc.) | ❌ No | ❌ No | More graph simplification algorithms; no on-the-fly callgraph construction |
| **AserPTA** | `aser-aa` | Inclusion-based | ❌ No | ✅ Yes (CI, 1-CFA, 2-CFA, Origin) | ✅ Yes | ✅ Yes (AserPTA) | On-the-fly callgraph construction; supports both field-insensitive and field-sensitive modes |
| **CFL (LLVM)** | - | CFL-reachability | ❌ No | ❌ No | - | ✅ Yes (LLVM) | Via alias wrapper (libLLVMAnalysis) |
| **DDA** | `dda` | Demand-driven value-flow | ✅ Yes | Optional (`FlowDDA` / `ContextDDA`) | ✅ Yes | ❌ No | Migrated from SVF concepts, but reimplemented natively in Lotus |
| **DyckAA** | - | Unification-based | ❌ No | ❌ No | - | ❌ No | - |
| **Dynamic** | - | Dynamic | - | - | - | ❌ No | Runtime analysis |
| **FPA** | `fpa` | Type-based | - | - | - | ❌ No | Function pointer analysis |
| **LotusAA** | `lotus-aa` | Inclusion-based | ✅ Yes | ✅ Yes | - | ❌ No | Native Lotus analysis; flow- and context-sensitive |
| **seadsa** | `sea-dsa-dg`, `seadsa-tool` | Unification-based | ❌ No | ✅ Yes | - | ✅ Yes (SeaDsa) | Context-sensitive heap (heap cloning) |
| **SRAA** | - | Range-based | ❌ No | ❌ No | - | ✅ Yes (CGO'17 SRAA) | Flow- and context-insensitive |
| **UnderApproxAA** | - | Pattern-based | - | - | - | ❌ No | Must-alias analysis |
| **AllocAA** | - | - | - | - | - | ❌ No | - |
| **TPA** | `tpa` | Inclusion-based | ✅ Yes | ✅ Yes (k-limiting) | - | ❌ No | Flow- and context-sensitive with k-limiting |

## Context Sensitivity Variants

| Analysis | Context Variants Supported |
|----------|---------------------------|
| **SparrowAA** | CI (context-insensitive), 1-CFA, 2-CFA |
| **AserPTA** | CI, 1-CFA, 2-CFA, Origin-sensitive |
| **DDA** | Flow-sensitive CI (`FlowDDA`), flow-sensitive CS (`ContextDDA`) |
| **LotusAA** | Context-sensitive (details not specified) |
| **seadsa** | Context-sensitive with heap cloning |
| **TPA** | Context-sensitive with k-limiting |

## Key Differences: SparrowAA vs AserPTA

| Feature | SparrowAA | AserPTA |
|---------|-----------|---------|
| **Callgraph Construction** | No on-the-fly construction | On-the-fly construction (default) |
| **Graph Simplification** | More algorithms integrated | Standard |
| **Field Sensitivity** | Field-insensitive only | Both field-insensitive and field-sensitive modes |
| **Context Variants** | CI, 1-CFA, 2-CFA | CI, 1-CFA, 2-CFA, Origin-sensitive |

## Analysis Characteristics Summary

| Characteristic | Analyses |
|----------------|----------|
| **Inclusion-based** | SparrowAA, AserPTA, LotusAA, TPA |
| **Demand-driven** | DDA |
| **Unification-based** | DyckAA, seadsa |
| **Flow-sensitive** | DDA, LotusAA, TPA |
| **Context-sensitive** | SparrowAA, AserPTA, DDA (ContextDDA), LotusAA, seadsa, TPA |
| **Field-sensitive** | AserPTA (optional), DDA, LotusAA, etc. |
| **Specialized** | FPA (function pointers), UnderApproxAA (must-alias), Dynamic (runtime), SRAA (range-based) |

## Subdirectories

`lib/Alias/` contains both native Lotus analyses and wrappers around external implementations.

- `DDA/`: demand-driven pointer analysis infrastructure, including `FlowDDA` and `ContextDDA`. The design was originally inspired by SVF's demand-driven analyses, but the code in this tree was migrated and reimplemented by the Lotus team and should be treated as a native Lotus subdirectory rather than a vendored third-party component.
- `DFPA/`: indirect-call target analysis with coarse whole-program propagation
  and demand-driven refinement.
- `DyckAA/`: unification-based alias analysis using Dyck-style constraints.
- `seadsa/`: SeaDsa-based heap and alias analysis integration.

## Comparing precision and soundness: metrics

To compare pointer analyses (e.g. SparrowAA vs TPA vs AserPTA), use the built-in **metrics** layer under `lib/Alias/Metrics/`:

- **Design**: See [METRICS.md](METRICS.md) for the rationale and mapping from Java points-to metrics (e.g. hybrid context-sensitivity, PLDI’13) to C/C++.
- **API**: `PointerAnalysisMetrics` and `collectMetricsFromWrapper(AliasAnalysisWrapper&, Module&, PointerAnalysisMetrics&)` in `include/Alias/Metrics/PointerAnalysisMetrics.h`.
- **Metrics**: Points-to set size (avg/median/max), call-graph edges, indirect-call sites and “poly” indirect calls (sites with >1 target).

**Which analyses support which metrics (via the wrapper):**

| Analysis | Points-to size | Indirect-call resolution |
|----------|----------------|---------------------------|
| SparrowAA, AserPTA | ✅ Full | ✅ |
| TPA | ✅ Size only | ✅ |
| DyckAA, UnderApprox, CFL* | ❌ | ❌ |
| Combined | ✅ (via Andersen) | ✅ (via Andersen) |

LotusAA and FPA are separate tools and are not backends in `AliasAnalysisWrapper`; SeaDsa/AllocAA/etc. are not yet integrated in the wrapper. See [METRICS.md](METRICS.md) for the full support table.

- **High-level clients** (taint, use-after-free, ref-count) need more than alias/points-to (mod/ref, DFA, etc.) and live outside this metrics layer.
