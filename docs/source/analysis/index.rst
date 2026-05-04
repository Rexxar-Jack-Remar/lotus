Analysis Framework
==================

This section covers the core analysis components and frameworks in Lotus.

Lotus provides several reusable analysis utilities and frameworks under
``lib/Analysis`` and adjacent subsystem modules such as ``lib/Concurrency``.
These components complement the alias analyses and high-level analyzers such
as CLAM (numerical abstract interpretation) and SymAbsAI (symbolic
abstraction + abstract interpretation) built in ``lib/Verification``.

Overview
--------

At a glance:

- **CFG** (``lib/Analysis/CFG``): Control Flow Graph utilities for reachability,
  dominance, and structural reasoning. See :doc:`cfg`.
- **Concurrency** (``lib/Concurrency``): Thread-aware analyses for
  multi-threaded code (MHP, lock sets, thread modeling). See :doc:`concurrency`.
- **DebugInfo** (``lib/Analysis/DebugInfo``): Source-location and metadata
  extraction support. See :doc:`debug_info`.
- **Loop** (``lib/Analysis/Loop``): Loop-dependence, iteration-space, and
  transformation-oriented loop analyses. See :doc:`loop`.
- **NullPointer** (``lib/Analysis/NullPointer``): A family of nullness and
  null-flow analyses. See :doc:`null_pointer`.
- **TypeHirarchy** (``lib/Analysis/TypeHirarchy``): Type-hierarchy and vtable
  recovery for object-oriented code. See :doc:`type_hierarchy`.


Higher-level analyzers such as CLAM and SymAbsAI build on these components;
see :doc:`../verification/clam` and :doc:`../verification/symabs-ai` for
details.

.. toctree::
   :maxdepth: 2

   cfg
   concurrency
   debug_info
   loop
   null_pointer
   type_hierarchy
