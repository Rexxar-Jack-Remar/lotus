Inter-Procedural Optimizations
==============================

This page summarizes inter-procedural optimizations implemented in
``lib/Optimization/IPO/`` plus ``lib/Optimization/Scalar/AggressiveInliner.cpp`` and exposed via
the ``lotus-opt`` tool.

**Implementation Location**: ``lib/Optimization/IPO/``

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

- **AInliner** (``lib/Optimization/Scalar/AggressiveInliner.cpp``)
  Aggressive inliner to simplify call boundaries and enable downstream IP
  optimizations.
- **IPDeadStoreElimination** (``lib/Optimization/IPO/IPDeadStoreElimination.cpp``)
  Removes inter-procedurally provable dead stores.
- **IPRedundantLoadElimination** (``lib/Optimization/IPO/IPRedundantLoadElimination.cpp``)
  Eliminates redundant loads across function boundaries when safe.
- **IPStoreSinking** (``lib/Optimization/IPO/IPStoreSinking.cpp``)
  Sinks stores inter-procedurally to reduce redundant writes.
- **IPStoreToLoadForwarding** (``lib/Optimization/IPO/IPStoreToLoadForwarding.cpp``)
  Forwards values from stores to later loads across calls when possible.
- **ModuleOptimizer** (``lib/Optimization/Pipeline/ModuleOptimizer.cpp``)
  Driver that wires the above passes into a module-level pipeline.

Notes
-----

- ``lib/Optimization/README.md`` tracks optimization-specific references and
  evaluation notes for select passes (e.g., prefetching, LICM).
- :doc:`../../optimization/ip` documents the dedicated ``lib/Optimization/IPO/``
  subtree.
