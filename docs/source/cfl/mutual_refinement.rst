Mutual Refinement for CFL Reachability
======================================

``include/CFL/MutualRefinement/`` and ``lib/CFL/MutualRefinement/`` contain a
mutual-refinement implementation for CFL reachability experiments.

**Location**: ``include/CFL/MutualRefinement/``,
``lib/CFL/MutualRefinement/``

**Main components**:

- ``Grammar`` stores the integer-encoded grammar.
- ``Graph`` stores the encoded graph instance.
- ``IntPairHasher`` supports the compact map/set structures used internally.
- ``MutualRefinementMain.cpp`` provides the standalone driver.

The implementation is best treated as a focused research component within the
broader CFL subsystem.

See also :doc:`cfl_components`.
