Inter-Procedural Optimizations
==============================

This page summarizes inter-procedural optimizations implemented in
``lib/Optimization/IP/`` plus ``lib/Optimization/AInliner.cpp`` and exposed via
the ``lotus-opt`` tool.

**Implementation Location**: ``lib/Optimization/IP/``

Tool
----

- **Binary**: ``lotus-opt``
- **Source**: ``tools/optimization/lotus-opt.cpp``
- **Selection flags**: ``-ainline``, ``-ipdse``, ``-ip-rle``, ``-ip-sink``,
  ``-ip-forward``, or ``-ip-all`` to run all of them.

Example:

.. code-block:: bash

   build/bin/lotus-opt -ip-all input.bc -o optimized.bc

Passes
------

- **AInliner** (``lib/Optimization/AInliner.cpp``)
  Aggressive inliner to simplify call boundaries and enable downstream IP
  optimizations.
- **IPDeadStoreElimination** (``lib/Optimization/IP/IPDeadStoreElimination.cpp``)
  Removes inter-procedurally provable dead stores.
- **IPRedundantLoadElimination** (``lib/Optimization/IP/IPRedundantLoadElimination.cpp``)
  Eliminates redundant loads across function boundaries when safe.
- **IPStoreSinking** (``lib/Optimization/IP/IPStoreSinking.cpp``)
  Sinks stores inter-procedurally to reduce redundant writes.
- **IPStoreToLoadForwarding** (``lib/Optimization/IP/IPStoreToLoadForwarding.cpp``)
  Forwards values from stores to later loads across calls when possible.
- **ModuleOptimizer** (``lib/Optimization/ModuleOptimizer.cpp``)
  Driver that wires the above passes into a module-level pipeline.

Notes
-----

- ``lib/Optimization/README.md`` tracks optimization-specific references and
  evaluation notes for select passes (e.g., prefetching, LICM).
- :doc:`../../optimization/ip` documents the dedicated ``lib/Optimization/IP/``
  subtree.
