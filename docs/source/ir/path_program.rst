Path Program
============

Path programs are control-flow abstractions induced by concrete execution
paths, aligned with the PLDI 2007 *Path Invariants* approach. Each path
program captures the sequence of program locations and control-flow transitions
visited along a single concrete path.

**Location**: ``lib/Verification/PathProgram/``

**Headers**: ``include/Verification/PathProgram/``

Overview
--------

A path program represents a set of concrete traces that follow the same
sequence of control-flow decisions. The representation models:

- **Locations** — the program locations (basic blocks) visited by the path.
- **Transitions** — the exact control-flow edges taken between locations.

The infrastructure builds path programs from LLVM IR by:

1. Extracting a concrete trace through a function (sequence of basic blocks).
2. Constructing the corresponding ``PathProgram`` object with the trace's
   control-flow skeleton.
3. Optionally compressing the representation using ``CompressedCfg``.

Components
----------

- ``PathProgram`` — Core data structure: ordered list of trace transitions and
  the induced control-flow graph.
- ``PathProgramBuilder`` — Constructs ``PathProgram`` instances from LLVM
  functions and trace information.
- ``PathProgramView`` — Provides a read-only view over a path program for
  downstream analyses.
- ``CompressedCfg`` — Compressed representation of the path program's CFG for
  efficient storage and traversal.

This subsystem is used by verification analyses that need path-specific
reasoning, such as invariant inference and path-sensitive property checking.

See Also
--------

- :doc:`index` — IR subsystem overview
- :doc:`../verification/sifa` — SIFA verification framework
