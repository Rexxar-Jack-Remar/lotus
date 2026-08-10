IFDS Taint Checker
==================

The ``taint`` subcommand runs Lotus's interprocedural IFDS taint analysis.
It follows user-configured data from source functions to sink functions.

**Frontend**: ``lotus-check taint``

**Implementation**: ``tools/checker/lotus-check-taint.cpp`` and
``include/Dataflow/IFDS/Analyses/IFDSTaintAnalysis.h``

Scope
-----

Use this checker for configurable source-to-sink queries, such as untrusted
input reaching a command-execution API.  Supply project-specific sources and
sinks when the defaults do not describe the program.

This is distinct from the auxiliary taint facts used by ``kint`` to focus
numerical-bug analysis, and from the taint tracking embedded in ``pulse`` and
``symex``.  Those engines report their own bug classes; they are not a
replacement for an explicitly configured IFDS source-to-sink query.

Usage
-----

.. code-block:: bash

   # Run with the default source and sink configuration.
   ./build/bin/lotus-check taint input.bc

   # Add project-specific source and sink functions.
   ./build/bin/lotus-check taint input.bc \
     --sources=recv,getenv --sinks=system,execve

   # Select an alias-analysis backend and show source/sink tagging details.
   ./build/bin/lotus-check taint input.bc --aa=dyck --verbose --max-results=20

Important options
-----------------

* ``--sources=<name[,name...]>`` adds source functions.
* ``--sinks=<name[,name...]>`` adds sink functions.
* ``--aa=<kind>`` selects the alias-analysis backend; the default is ``dyck``.
* ``--max-results=<n>`` limits detailed results printed by the checker.
* ``--verbose`` prints module details and source/sink tagging at call sites.
* ``--micro-bench`` adds the conventional ``source`` and ``sink`` functions
  for benchmark evaluation.

Reporting
---------

The IFDS frontend prints flow results directly.  Unlike the native checker
frontends, it does not currently export findings through ``BugReportMgr``;
therefore the shared ``--report-json`` and ``--report-sarif`` report pipeline
does not produce taint-flow reports from this subcommand.

See also
--------

* :ref:`Choosing a Checker <choosing-a-checker>` – bug-class to engine guide
* :doc:`../dataflow/ifds_ide` – IFDS/IDE framework
* :doc:`../annotation/taint_config` – reusable taint configuration
