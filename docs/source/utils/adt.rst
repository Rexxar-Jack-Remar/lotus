Utility ADTs and Worklists
==========================

``include/Utils/ADT/`` provides the reusable container and worklist layer used
across analyses.

**Main components**:

- ``DisjointSet`` and ``UnionFind`` for partition maintenance.
- ``ImmutableMap``, ``ImmutableSet``, and ``ImmutableTree`` for persistent data.
- ``PriorityWorkList`` and ``TwoLevelWorkList`` for solver scheduling.
- ``TreeStream`` and related iterator adapters for structured traversal.

These headers are used heavily by the dataflow, alias, and solver subsystems.

See also :doc:`utilities`.
