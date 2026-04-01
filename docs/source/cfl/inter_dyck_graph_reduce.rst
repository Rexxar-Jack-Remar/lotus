Interleaved Dyck Graph Reduction
================================

``include/CFL/InterDyckGraphReduce/`` and its matching ``lib`` subtree provide
graph reduction algorithms for interleaved-Dyck reachability problems.

**Location**: ``include/CFL/InterDyckGraphReduce/``,
``lib/CFL/InterDyckGraphReduce/``

**Main components**:

- ``CFLGraph`` stores the labeled reachability instance.
- ``CFLReach`` performs the reduction and query process.
- ``SummaryGraph`` and ``MergedEdges`` compact intermediate state.

This code is aimed at research-style graph reductions rather than day-to-day
LLVM pass use.

See also :doc:`cfl_components`.
