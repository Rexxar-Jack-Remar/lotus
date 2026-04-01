LLVM Utility Layer
==================

``include/Utils/LLVM/`` and ``lib/Utils/LLVM/`` provide the common LLVM-facing
support code reused by front-ends and analysis passes.

**Main components**:

- ``IO/ReadIR`` and ``IO/WriteIR`` for reading and writing LLVM modules.
- ``InstructionUtils`` and ``Demangle`` for IR inspection.
- ``GraphWriter`` and ``LLVMBgl`` for graph traversal and visualization.
- ``Statistics``, ``RecursiveTimer``, and ``Log`` for diagnostics.

This layer is the shared glue between Lotus analyses and LLVM infrastructure.

See also :doc:`utilities`.
