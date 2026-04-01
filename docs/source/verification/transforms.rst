Verification Transformation Passes
==================================

``include/Verification/Transform/`` and ``lib/Verification/Transform/`` provide
IR rewrites that prepare programs for model checking and abstract
interpretation.

**Representative passes**:

- ``MakeNondetPass`` and ``DeleteUndefinedPass`` normalize undefined behavior.
- ``InstrumentAllocPass`` and ``PrepareOverflowsPass`` expose verification
  checks explicitly.
- ``InitializeUninitializedPass`` and ``BreakInfiniteLoopsPass`` simplify the
  state space.
- loop and API rewriting passes adapt LLVM IR to backend expectations.

These transforms are typically used before handing a module to verifier tools.

See also :doc:`../user_guide/instrumentation_passes` and :doc:`backend`.
