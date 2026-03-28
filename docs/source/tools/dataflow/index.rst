Dataflow Tools
==============

This page documents the testing and comparison tools under ``tools/dataflow/``.
They are primarily intended for validating and comparing Lotus dataflow engines
over the same LLVM IR input.

Overview
--------

The current front-ends focus on intraprocedural or IFDS-style benchmark runs and
emit machine-readable summaries that are easy to diff in tests.

lotus-dfa
---------

Differential-testing front-end that compares multiple engines on the same
analysis problem.

**Binary**: ``lotus-dfa``

**Source**: ``tools/dataflow/lotus-dfa-diff.cpp``

**Usage**:

.. code-block:: bash

   ./build/bin/lotus-dfa --analysis=liveness input.bc
   ./build/bin/lotus-dfa --analysis=constant_prop --engine=all input.bc
   ./build/bin/lotus-dfa --analysis=reaching_defs --engine=ifds input.bc

Important options:

- ``--analysis=liveness|reaching_defs|uninitialized|constant_prop|available_exprs|reachable``
- ``--engine=elim|mono|ifds|all``
- ``--elim-method=state|adt-simple|adt-delayed``
- ``--out-dir=<dir>``

lotus-dfa-elim
--------------

Standalone front-end for the elimination-based APA engine.

**Binary**: ``lotus-dfa-elim``

**Source**: ``tools/dataflow/lotus-dfa-elim.cpp``

Supports the same ``--analysis`` space as ``lotus-dfa`` plus
``--elim-method`` for choosing the elimination solver variant.

lotus-dfa-mono
--------------

Standalone front-end for the Mono engine.

**Binary**: ``lotus-dfa-mono``

**Source**: ``tools/dataflow/lotus-dfa-mono.cpp``

Supported analyses:

- ``liveness``
- ``reachable``
- ``constant_prop``
- ``uninitialized``

lotus-dfa-ifds
--------------

Standalone front-end for IFDS-based analyses.

**Binary**: ``lotus-dfa-ifds``

**Source**: ``tools/dataflow/lotus-dfa-ifds.cpp``

Supported analyses:

- ``reaching_defs``
- ``uninitialized``

See also
--------

- See :doc:`../../dataflow/apa`, :doc:`../../dataflow/mono`, and
  :doc:`../../dataflow/ifds_ide` for the underlying engines.
