# IR Library

IR representations and transformations for LLVM.

| Subdir | Purpose |
|--------|---------|
| **GSA** | Gated SSA (gamma nodes, control dependence). Thinned GSA, optional PHI replacement. |
| **ICFG** | Interprocedural CFG: call/return edges, call graph. Used by IFDS/IDE, WPDS, PDG. |
| **MemorySSA** | Memory SSA over Sea-DSA ShadowMem. Def-use chains, interprocedural memory tracking. |
| **PDG** | Program dependence graph. Data/control deps, slicing, context-sensitive slicing. |
| **SSI** | Static Single Information. Sigma functions, dual dominance, path-sensitive representation. |
