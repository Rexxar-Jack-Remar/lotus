# VASCO for Lotus

This directory hosts a faithful C++ migration of the original VASCO
inter-procedural value-context framework (SOAP 2013).

- SOPA 13: Interprocedural Data Flow Analysis in Soot using Value Contexts.
Rohan Padhye and Uday P. Khedker.

The migration keeps the original framework structure:

- `ProgramRepresentation` abstracts entry points, CFGs, and call resolution.
- `Context` models value contexts keyed by method plus entry/exit flow.
- `ContextTransitionTable` records call-site to callee-context edges.
- `InterProceduralAnalysis` provides the shared context/worklist machinery.
- `ForwardInterProceduralAnalysis` and `BackwardInterProceduralAnalysis`
  implement the core algorithm from Figure 1 of the paper.

The implementation is header-only to stay generic over Lotus method, node,
and lattice types.

LLVM-specific surfaces are provided for the migrated client-analysis layer:

- `LLVM/DefaultLLVMProgramRepresentation.h` adapts LLVM IR to the generic
  VASCO API using instruction-level CFGs and direct-call resolution.
- `Clients/LLVMSignAnalysis.h` ports the original sign-analysis example to
  LLVM IR values and instructions.
- `Clients/LLVMCopyConstantAnalysis.h` ports the original copy-constant
  propagation example to LLVM IR.
- `Clients/LLVMPointsToAnalysis.h` ports the original points-to/call-target
  client to LLVM IR using allocation-site objects, field-sensitive byte
  offsets, and the legacy VASCO call handling API that the original Java
  client relied on.

Current LLVM scope and known limits:

- The generic value-context framework from the paper is present, including
  forward/backward inter-procedural propagation, context reuse by entry/exit
  values, default-call-site tracking, and meet-over-valid-paths aggregation.
- The default LLVM program representation is intentionally lightweight: it uses
  an instruction-level intraprocedural CFG and default direct-call resolution,
  with support for custom target resolvers supplied by clients.
- Unknown/indirect calls are modeled as default sites and the sample LLVM
  analyses conservatively degrade returned values at such calls.
- The original Java `vasco.callgraph` client now has an LLVM migration with a
  dedicated C++ implementation under `lib/Dataflow/VASCO/`: it models pointer
  locals, globals, heap allocations, field-sensitive GEP/load/store effects,
  direct calls, and indirect calls through function-pointer values.
- The LLVM migration is still lighter-weight than the Soot implementation in a
  few places: external/native callees are summarized conservatively, arrays are
  tracked by byte-offset rather than per-element explosion, and there is no
  attempt to recreate JVM-specific models or Soot Scene call-graph objects.
- Production caveat: this client is a usable LLVM migration and now field-
  sensitive for constant-offset memory accesses, but it is still conservative
  around unknown offsets, heap-modeling beyond malloc-like allocation sites,
  and advanced external-library summaries.
- For whole-program LLVM analysis that needs richer indirect-call resolution,
  clients are expected to plug in Lotus alias/call-graph infrastructure through
  the custom resolver hook rather than rely on the default representation.
