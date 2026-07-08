Symbolic Execution Checker
==========================

The symbolic-execution checker is exposed through the ``lotus-check symex``
subcommand and is backed by the top-level ``SymbolicExecution`` subsystem.

**Engine Location**: ``lib/SymbolicExecution/``

**Headers**: ``include/SymbolicExecution/``

**Tool Frontend**: ``tools/checker/lotus-check-symex.cpp``

Overview
--------

The engine performs path-sensitive symbolic execution over the guarded
value-flow graph. It tracks symbolic scalar values, access paths, guarded
memory facts, taint-derived facts, and path conditions, then uses SMT-backed
feasibility checks before emitting bug reports.

The main build target is ``CanarySymbolicExecution``. The target remains
separate from ``lib/Analysis`` because the subsystem owns a full driver,
symbolic state model, solver bridge, memory modeling layer, taint model, and
LLVM pass wrapper.

Core Components
---------------

* ``AnalysisDriver`` – Coordinates whole-module symbolic execution and summary
  scheduling.
* ``AnalysisState`` – Represents symbolic memory, guarded values, points-to
  items, summaries, and bug queries.
* ``ProgramVar`` and ``Property*`` – Name symbolic values and represent scalar
  properties such as offsets, sizes, and affine expressions.
* ``ConstraintRepr`` and ``PathCondSolver`` – Encode symbolic predicates and
  discharge path-feasibility queries.
* ``MemoryAPI`` and ``GVFGUtility`` – Connect symbolic execution to allocation
  modeling, data layout, library summaries, and GVFG construction.
* ``TaintModel`` – Provides source, transfer, and sink facts used by
  taint-sensitive bug checks.
* ``SymbolicExecutionWrapper`` – Integrates the engine with the LLVM pass
  pipeline and checker report infrastructure.

Usage
-----

.. code-block:: bash

   ./build/bin/lotus-check symex input.bc
   ./build/bin/lotus-check symex input.bc --symex-checkers=null-deref,uaf

Tests
-----

Focused unit tests live under ``tests/unit/SymbolicExecution`` and build
against ``CanarySymbolicExecution``.

See Also
--------

* :doc:`../symbolic_execution/index` – Symbolic execution engine documentation
* :doc:`index` – Checker framework overview
* :doc:`../tools/checker/index` – ``lotus-check`` command-line frontend
