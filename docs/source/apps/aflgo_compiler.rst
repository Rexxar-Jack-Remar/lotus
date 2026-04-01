AFLGo Compiler Plugin
=====================

``include/Fuzzing/AFLGoCompiler/`` and ``lib/Fuzzing/AFLGoCompiler/`` implement
the compile-time LLVM plugin used to inject directed-fuzzing targets.

**Location**: ``include/Fuzzing/AFLGoCompiler/``,
``lib/Fuzzing/AFLGoCompiler/``

**Main components**:

- ``AFLGoTargetInjectionPass`` inserts target metadata during compilation.
- ``Plugin.cpp`` registers the pass as an LLVM plugin.

This layer prepares IR so later link-time instrumentation can compute and emit
distance-guided feedback.

See also :doc:`fuzzing_analysis` and :doc:`aflgo_linker`.
