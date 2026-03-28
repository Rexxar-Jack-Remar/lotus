Analysis Framework
==================

This section covers the core analysis components and frameworks in Lotus.

Lotus provides several reusable analysis utilities and frameworks under
``lib/Analysis``. These components complement the alias analyses and
high-level analyzers such as CLAM (numerical abstract interpretation) and
SymAbsAI (symbolic abstraction + abstract interpretation) built in ``lib/Verification``.

Overview
--------

At a glance:

- **CFG** (``lib/Analysis/CFG``): Control Flow Graph utilities for reachability,
  dominance, and structural reasoning. See :doc:`cfg`.
- **Concurrency** (``lib/Analysis/Concurrency``): Thread-aware analyses for
  multi-threaded code (MHP, lock sets, thread modeling). See :doc:`concurrency`.
- **Crypto** (``lib/Analysis/Crypto``): Constant-time programming analysis for
  cryptographic code. See :doc:`crypto`.
- **DebugInfo** (``lib/Analysis/DebugInfo``): Source-location and metadata
  extraction support. See :doc:`debug_info`.
- **NullPointer** (``lib/Analysis/NullPointer``): A family of nullness and
  null-flow analyses. See :doc:`null_pointer`.
- **Spectre** (``lib/Analysis/Spectre``): Cache speculation analysis for
  detecting Spectre vulnerabilities. See :doc:`spectre`.
- **TypeHirarchy** (``lib/Analysis/TypeHirarchy``): Type-hierarchy and vtable
  recovery for object-oriented code. See :doc:`type_hierarchy`.


Higher-level analyzers such as CLAM and SymAbsAI build on these components;
see :doc:`../verification/clam` and :doc:`../verification/symabs-ai` for
details.

.. toctree::
   :maxdepth: 2

   cfg
   concurrency
   crypto
   debug_info
   null_pointer
   spectre
   type_hierarchy
