Loop Analysis Framework
=======================

``include/Analysis/Loop/`` and ``lib/Analysis/Loop/`` implement the loop-side
analysis stack used by several optimization and verification workflows.

**Main components**:

- ``FunctionLoopAnalyses`` coordinates per-function loop analyses.
- ``LoopContent`` and ``LoopEnvironment`` summarize loop structure and context.
- ``LoopDependenceGraph`` and ``LoopSCCDAG`` model intra-loop dependencies.
- ``LoopIterationSpaceAnalysis`` reasons about loop bounds and iteration space.
- ``MemoryCloningAnalysis`` and ``SCCDAGAttrs`` support loop transformations.

This subsystem builds on LLVM loop information but adds Lotus-specific program
analysis structures.

See also :doc:`cfg` and :doc:`../optimization/index`.
