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

Experiment workflow
-------------------

Prepare the grammar and graph using the integer encodings expected by the
component, then run the standalone driver to evaluate the refinement process.
The local ``Grammar`` and ``Graph`` types are intentionally specialized; use a
different CFL frontend when an application needs a stable LLVM-facing API or
human-readable input format.  Record the encoding and benchmark corpus when
comparing refinement strategies.

See also :doc:`cfl_components`.
