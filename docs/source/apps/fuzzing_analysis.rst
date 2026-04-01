Directed Fuzzing Analyses
=========================

``include/Fuzzing/Analysis/`` and ``lib/Fuzzing/Analysis/`` provide the core
distance and target-discovery analyses used by the Lotus directed fuzzing stack.

**Location**: ``include/Fuzzing/Analysis/``, ``lib/Fuzzing/Analysis/``

**Main analyses**:

- ``AFLGoBasicBlockDistanceAnalysis`` computes block-to-target distances.
- ``AFLGoFunctionDistanceAnalysis`` computes function-level distances.
- ``ExtendedCallGraphAnalysis`` enriches the call graph with pointer-analysis
  information.
- ``AFLGoTargetDetectionAnalysis`` discovers or validates fuzzing targets.
- ``DAFLAnalysis`` adds data-dependence guidance.

These analyses are consumed by the compiler and linker plugins documented in
:doc:`aflgo_compiler` and :doc:`aflgo_linker`.

See also :doc:`fuzzing_support`.
