Saber Checker
=============

The ``Saber`` subsystem implements source-sink bug checking on top of Lotus IR
and value-flow graphs.

**Headers**: ``include/Checker/Saber/``

**Implementation**: ``lib/Checker/Saber/``

**Tool**: ``lotus-saber`` in ``tools/checker/lotus-saber.cpp``

Overview
--------

Saber-style analysis tracks source-sink relationships over the sparse
value-flow graph to detect resource-management bugs. In the current tree it is
used primarily for memory leaks, double-free bugs, and file-descriptor leaks.

Main components
---------------

- ``LeakChecker`` checks unmatched allocations and partial leaks.
- ``DoubleFreeChecker`` reports repeated frees on the same path.
- ``FileChecker`` handles file-descriptor style resources.
- ``SaberCheckerAPI`` and ``SrcSnkSolver`` expose reusable source-sink solving
  infrastructure.

Typical usage
-------------

.. code-block:: bash

   ./build/bin/lotus-saber input.bc
   ./build/bin/lotus-saber --all input.bc
   ./build/bin/lotus-saber --double-free --file input.bc

Behavior
--------

- With no explicit checker flags, ``lotus-saber`` defaults to leak checking.
- When multiple checks are enabled, the tool tries to build and reuse shared
  SVFG and ICFG state across checkers.

See also
--------

- See :doc:`../tools/checker/index` for the tool overview.
- See :doc:`pulse` for a different memory-safety checker family.
