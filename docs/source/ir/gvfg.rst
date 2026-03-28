GVFG
====

The Guarded Value-Flow Graph (GVFG) extends value-flow reasoning with explicit
guards and block conditions.

**Headers**: ``include/IR/GVFG/``

**Implementation**: ``lib/IR/GVFG/``

Overview
--------

GVFG models value propagation while preserving predicate information that
constrains whether a flow edge is feasible. This is useful for analyses that
need more path sensitivity than a plain value-flow graph but do not want to
enumerate full paths.

Main components
---------------

- ``GuardedValueFlowGraph`` stores nodes, interface nodes, producers, and block
  conditions.
- ``GuardedValueFlowBuilder`` constructs the graph from LLVM IR and Lotus IR
  infrastructure.
- ``GuardedValueFlowSolver`` reasons over guarded flows.
- ``GuardedValueFlowSerializer`` emits graph data for inspection or testing.
- ``LotusAdapter`` bridges GVFG with other Lotus representations.

Use cases
---------

- Guard-aware dataflow reasoning.
- Querying feasible flows with branch conditions attached.
- Exporting richer value-flow graphs for experiments and debugging.

See also
--------

- See :doc:`svfg` for the unguarded sparse value-flow graph.
- See :doc:`pdg` for dependence-based querying.
