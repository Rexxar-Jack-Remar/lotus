Interprocedural MemorySSA Optimizations
=======================================

``lib/Optimization/IP/`` contains the interprocedural ShadowMem and MemorySSA
optimizations used by the optimization tooling.

**Location**: ``lib/Optimization/IP/``

**Main passes**:

- ``IPDeadStoreElimination`` removes stores that never reach an observable use.
- ``IPRedundantLoadElimination`` removes repeated loads with equivalent reaching
  memory state.
- ``IPStoreSinking`` moves stores closer to their first real use.
- ``IPStoreToLoadForwarding`` replaces loads with unique reaching stored values.

These passes are exposed through the interprocedural optimization front-end.

See also :doc:`index` and :doc:`../tools/optimization/interprocedural`.
