Control Flow Support
====================

``Dataflow/ControlFlow`` provides reusable CFG abstractions for dataflow engines.

**Headers**: ``include/Dataflow/ControlFlow/``

Overview
--------

This subsystem defines lightweight intraprocedural and interprocedural control
flow graph interfaces that decouple solver code from raw LLVM traversal.

Main components
---------------

- ``FlowDirection`` models forward versus backward analyses.
- ``IntraCFG`` and ``LLVMIntraCFG`` provide function-local control flow views.
- ``InterCFG`` and ``LLVMInterCFG`` extend the model across calls.

Why it exists
-------------

- Shared CFG abstractions reduce duplicated traversal logic across solvers.
- Engines can operate over a small stable interface instead of directly on LLVM.
- The same framework supports forward and backward analyses.

See also
--------

- See :doc:`apa`, :doc:`mono`, and :doc:`ifds_ide` for clients built on top of
  control-flow abstractions.
