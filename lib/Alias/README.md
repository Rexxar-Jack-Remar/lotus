# The Phenix Alias Analsyis Toolkits

This directory contains various alias analysis implementations and toolkits used in the Phenix project.

## Analysis Comparison Table

| Analysis | Tool/Command | Analysis Type | Flow-Sensitive | Context-Sensitive | Field-Sensitive | Notes |
|----------|--------------|---------------|----------------|-------------------|-----------------|-------|
| **SparrowAA** | `sparrow-aa` | Inclusion-based | ❌ No | ✅ Yes (CI, 1-CFA, 2-CFA) | ❌ No | More graph simplification algorithms; no on-the-fly callgraph construction |
| **AserPTA** | `aser-aa` | Inclusion-based | ❌ No | ✅ Yes (CI, 1-CFA, 2-CFA, Origin) | ✅ Yes | On-the-fly callgraph construction; supports both field-insensitive and field-sensitive modes |
| **CFL (LLVM)** | - | CFL-reachability | ❌ No | ❌ No | - | Via alias wrapper (libLLVMAnalysis) |
| **DyckAA** | - | Unification-based | ❌ No | ❌ No | - | - |
| **Dynamic** | - | Dynamic | - | - | - | Runtime analysis |
| **FPA** | `fpa` | Type-based | - | - | - | Function pointer analysis |
| **LotusAA** | `lotus-aa` | Inclusion-based | ✅ Yes | ✅ Yes | - | Flow- and context-sensitive |
| **seadsa** | `sea-dsa-dg`, `seadsa-tool` | Unification-based | ❌ No | ✅ Yes | - | Context-sensitive heap (heap cloning) |
| **SRAA** | - | Range-based | ❌ No | ❌ No | - | Flow- and context-insensitive |
| **UnderApproxAA** | - | Pattern-based | - | - | - | Must-alias analysis |
| **AllocAA** | - | - | - | - | - | - |
| **TPA** | `tpa` | Inclusion-based | ✅ Yes | ✅ Yes (k-limiting) | - | Flow- and context-sensitive with k-limiting |

## Context Sensitivity Variants

| Analysis | Context Variants Supported |
|----------|---------------------------|
| **SparrowAA** | CI (context-insensitive), 1-CFA, 2-CFA |
| **AserPTA** | CI, 1-CFA, 2-CFA, Origin-sensitive |
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
| **Unification-based** | DyckAA, seadsa |
| **Flow-sensitive** | LotusAA, TPA |
| **Context-sensitive** | SparrowAA, AserPTA, LotusAA, seadsa, TPA |
| **Field-sensitive** | AserPTA (optional) |
| **Specialized** | FPA (function pointers), UnderApproxAA (must-alias), Dynamic (runtime), SRAA (range-based) |
