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
