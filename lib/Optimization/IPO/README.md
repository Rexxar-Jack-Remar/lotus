# Inter-procedural (IPO) optimizations

This directory contains inter-procedural memory optimizations that use ShadowMem/MemorySSA instrumentation:

- **IPDeadStoreElimination** — removes stores (and some global initializers) whose MemorySSA def-use chains never reach a shadow load; works across calls via shadow.mem.arg.*, shadow.mem.in/out.
- **IPRedundantLoadElimination** — eliminates redundant loads within a basic block using MemorySSA; conservative, relies on TLVars for interprocedural effects.
- **IPStoreSinking** — sinks stores closer to their uses within a basic block to reduce register pressure.
- **IPStoreToLoadForwarding** — replaces loads with the value from a reaching store by walking MemorySSA def-use chains across function boundaries.

These sources are built by `CanaryOptimizationIPO` (see `lib/Optimization/IPO/CMakeLists.txt`).
