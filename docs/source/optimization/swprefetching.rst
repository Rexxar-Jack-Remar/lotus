SWPrefetching (Software Prefetching)
====================================

**File**: ``lib/Optimization/Prefetch/``, ``include/Optimization/Prefetch/Prefetch.h``

Implements profile-guided software prefetching for indirect memory accesses.
This pass injects ``llvm.prefetch`` intrinsics to reduce cache misses.

Features
--------

- Profile-guided prefetch distance computation
- Supports Sample FDO profiles, user-provided LBR (Last Branch Record) distances,
  or user-specified LLM distances
- Discovers loop-carried induction variables that feed load addresses
- Clones dependence chains to compute future addresses before issuing prefetches

Command-line options
--------------------

.. code-block:: bash

   -prefetch-distance-provider=profile|lbr|llm|static
   -input-file=<filename>        # Sample profile (profile mode only)
   -dist=<value>                  # LBR distances (lbr mode, or fallback)
   -llm-dist=<value>              # LLM-provided distances (llm mode)

Distance providers
------------------

The pass can obtain prefetch distances from multiple sources:

- ``profile``: Read sample profiles from ``-input-file`` and query call target
  maps for distance hints.
- ``lbr``: Use the values passed to ``-dist`` as prefetch distances.
- ``llm``: Use values passed to ``-llm-dist`` as prefetch distances. This is a
  heuristic mode intended for experimentation.
- ``static``: Reserved for future static-analysis-driven distance estimation.

Algorithm
---------

The pass:

1. Loads profile hints (or uses the selected distance provider)
2. Discovers loop-carried induction variables feeding load addresses
3. Clones the dependence chain to compute a future address
4. Issues ``llvm.prefetch`` intrinsics at the computed distance

When to use
-----------

- Improve cache performance for memory-bound loops
- Reduce cache misses for indirect memory accesses
- Optimize hot paths identified by profiling

Related work
------------

- CGO 17: Software Prefetching for Indirect Memory Accesses (Sam Ainsworth, Timothy M. Jones)
- TOCS 19: Software Prefetching for Indirect Memory Accesses: A Microarchitectural Perspective
- EuroSys'22: PT-GET: profile-guided timely software prefetching

Benchmarks
----------

https://github.com/masabahmad/CRONO
