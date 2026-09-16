Dynamic Bidirected Dyck Reachability
====================================

``CanaryDynamicDyck`` directly ports the C++ dynamic algorithms from Li,
Satya, and Zhang's `Efficient Algorithms for Dynamic Bidirected
Dyck-Reachability <https://doi.org/10.1145/3498724>`_ (POPL 2022).

**Headers**: ``include/CFL/DynamicDyck/``

**Implementation**: ``lib/CFL/DynamicDyck/``

**Namespace**: ``lotus::cfl::dynamic_dyck``

Public graph, solver, file IO, and shared statistics headers live at the
module root. Backend headers and implementations share matching ``Detail/``
directories. The DOT parser has declarations in ``Detail/DotParser.h`` and
definitions in ``Detail/DotParser.cpp``; template/container support remains
header-only.

The original degree-based merging, fully compressed disjoint sets, indexed
linked worklists, weighted quotient updates, and acyclic recursive splitting
are retained. Original global state belongs to a solver instance. Bug fixes
reset preprocessing state, correct duplicate/missing-edge degree accounting,
and avoid invalid arrow parsing. The `2024 cycle correction
<https://arxiv.org/html/2401.03570v1>`_ motivates a conservative deletion
fallback: if the old affected quotient contains cycles, preprocess the
remaining original graph again. No original-paper update-time bound is claimed.

Library API
-----------

``Solver`` supports ``addVertex``, ``insertEdge``, ``deleteEdge``, ``apply``,
``connected``, ``representative``, ``components``, ``graph``, and
``statistics``. Each edge denotes an opening arc and its closing reverse;
updates operate on the pair. Duplicate insertion and absent deletion are
no-ops. Numeric vertex IDs are signed 64-bit integers; labels are unsigned
integers. Insertion creates endpoints; deletion does not. Queries involving
unknown vertices return false. Concurrent access requires synchronization
because representative queries compress paths.

.. code-block:: cpp

   #include "CFL/DynamicDyck/Solver.h"
   using namespace lotus::cfl::dynamic_dyck;
   Solver solver;
   solver.insertEdge({10, 30, 0});
   solver.insertEdge({20, 30, 0});
   assert(solver.connected(10, 20));
   solver.deleteEdge({30, 20, 0, Parenthesis::Close});
   assert(!solver.connected(10, 20));

``IO.h`` exposes ``runFiles`` for the original opaque string-ID workflow,
returning original timing results and final partitions. ``Graph.h`` offers
stricter numeric convenience parsers separately. Only one Dyck language is
supported; neutral and interleaved bracket constraints are outside this module.

Original benchmark workflow
---------------------------

.. code-block:: bash

   cmake --build build --target lotus-cfl-dynamic-dyck dynamic_dyck_test
   build/bin/lotus-cfl-dynamic-dyck 1 initial.dot updates.seq
   build/bin/lotus-cfl-dynamic-dyck 0 initial.dot updates.seq
   ctest --test-dir build -R dynamic_dyck --output-on-failure

``0`` selects recomputation after every input record, including no-ops;
``1`` selects dynamic updates. The CLI accepts original
``SOURCE->TARGET[label="op--TYPE"]`` / ``cp--TYPE`` graph records and
``A|D SOURCE TARGET LABEL`` sequence records, interning names and suffixes
as strings. Default stdout is one elapsed-seconds value and a trailing space,
with no newline, matching the original table scripts. Timing retains the
original scopes; ``--stats`` and ``--print-components`` add diagnostics.

Run the original table scripts directly; paths are resolved relative to the
repository, with ``DYCK_REACH_BINARY`` and ``DYCK_BENCHMARK_ROOT`` overrides:

.. code-block:: bash

   bash scripts/cfl/dynamic-dyck/gen_table3.sh
   bash scripts/cfl/dynamic-dyck/gen_table4.sh

The original C++ benchmark inputs live in
``benchmarks/real-world/CFL/DynamicDyck`` and the scripts in
``scripts/cfl/dynamic-dyck``. Unused backup snapshots are omitted.
The optional ``run_benchmarks.py`` runner
supports case selection, dry-run input validation, and partition comparison
without changing the original table ordering; ``--binary`` selects another
build. DDlog is not migrated.
