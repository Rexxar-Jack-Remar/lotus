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

Working with an instance
------------------------

Construct the labeled ``CFLGraph`` from the relation of interest, then use
``CFLReach`` to perform the reduction and obtain reachability information.
``SummaryGraph`` and ``MergedEdges`` are implementation-level compact
representations that reduce repeated work during solving.  Clients should
preserve the edge-label convention used to build the graph: a reduction cannot
recover nesting semantics that were not encoded in the input labels.

See also :doc:`cfl_components`.
