Saber Checker
=============

The ``Saber`` subsystem implements source-sink bug checking on top of Lotus IR
and value-flow graphs.

**Headers**: ``include/Checker/Saber/``

**Implementation**: ``lib/Checker/Saber/``

**Frontend**: ``lotus-check saber`` implemented by ``tools/checker/lotus-check-saber.cpp``

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

   ./build/bin/lotus-check saber input.bc
   ./build/bin/lotus-check saber input.bc --all
   ./build/bin/lotus-check saber input.bc --double-free --file

Behavior
--------

- With no explicit checker flags, ``lotus-check saber`` defaults to leak checking.
- When multiple checks are enabled, the tool tries to build and reuse shared
  SVFG and ICFG state across checkers.

Interpreting findings
---------------------

Saber reports source-to-sink relationships that satisfy its value-flow model.
Review the diagnostic trace together with the modeled allocation and library
semantics before treating a report as confirmed.  Calls without available
bodies or summaries can affect precision, so use the shared annotation and
alias configuration consistently when comparing runs or triaging results.

See also
--------

- See :doc:`../tools/checker/index` for the tool overview.
- See :doc:`pulse` for a different memory-safety checker family.
