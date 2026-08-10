Checker Tooling
===============

Subcommand registration and entry points for the ``lotus-check`` unified
checker frontend.

**Header**: ``include/Checker/Tooling/CheckerSubcommands.h``

**Implementation**: ``tools/checker/``

**Build target**: ``lotus-check``

Overview
--------

The Checker Tooling module provides the subcommand infrastructure for the
unified ``lotus-check`` binary. Each checker category registers an
``llvm::cl::SubCommand`` with a descriptive name and help text, enabling
dispatch from a single entry point.

Checker Subcommands
-------------------

**File**: ``CheckerSubcommands.h``

Defines inline accessor functions for each subcommand:

.. code-block:: cpp

   #include "Checker/Tooling/CheckerSubcommands.h"

   auto &sub = lotus::checker::tooling::genericSubCommand();
   auto &sub = lotus::checker::tooling::kintSubCommand();
   auto &sub = lotus::checker::tooling::taintSubCommand();
   auto &sub = lotus::checker::tooling::concurrencySubCommand();
   auto &sub = lotus::checker::tooling::pulseSubCommand();
   auto &sub = lotus::checker::tooling::fitxSubCommand();
   auto &sub = lotus::checker::tooling::saberSubCommand();
   auto &sub = lotus::checker::tooling::aeSubCommand();
   auto &sub = lotus::checker::tooling::symexSubCommand();

Available Subcommands
---------------------

+-------------------+-------------------------------------------+
| Subcommand        | Description                               |
+===================+===========================================+
| ``generic``       | Run declarative and registry-backed       |
|                   | checks                                    |
+-------------------+-------------------------------------------+
| ``kint``          | Run the KINT integer checker              |
+-------------------+-------------------------------------------+
| ``taint``         | Run the IFDS-based taint analysis         |
+-------------------+-------------------------------------------+
| ``concur``        | Run the concurrency checker suite         |
+-------------------+-------------------------------------------+
| ``pulse``         | Run the Pulse checker                     |
+-------------------+-------------------------------------------+
| ``fitx``          | Run the FiTx checker suite                |
+-------------------+-------------------------------------------+
| ``saber``         | Run the Saber checker                     |
+-------------------+-------------------------------------------+
| ``ae``            | Run the abstract-execution checker        |
+-------------------+-------------------------------------------+
| ``symex``         | Run the symbolic-execution checker        |
+-------------------+-------------------------------------------+

Usage
-----

.. code-block:: bash

   ./build/bin/lotus-check kint input.bc --check-all=true
   ./build/bin/lotus-check concur input.bc --check-data-races

See Also
--------

- :doc:`./index` — Checker framework overview
- :doc:`./kint` — KINT integer checker
- :doc:`./pulse` — Pulse biabductive checker
- :doc:`./saber` — Saber source-sink checker
- :doc:`./symex` — symbolic-execution checker
- :doc:`./taint` — IFDS taint checker
