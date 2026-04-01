Alias Specification Manager
===========================

``include/Alias/Spec/`` and ``lib/Alias/Spec/`` provide a unified specification
layer for library functions used by pointer and alias analyses.

**Main components**:

- ``AliasSpecManager`` loads and serves per-function specifications.
- ``FunctionCategory`` classifies library routines.
- ``AllocatorInfo``, ``CopyInfo``, ``ReturnAliasInfo``, and ``ModRefInfo`` model
  specialized behaviors that analyses need.

This layer complements the generic annotation subsystem by exposing a
pointer-analysis-oriented API.

See also :doc:`../annotation/modref`, :doc:`../annotation/pointer_effects`, and
:doc:`alias_analysis`.
