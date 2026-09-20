Generalized Points-to Graph Analysis
====================================

The GPG analysis is a bottom-up exhaustive pointer analysis based on
generalized points-to updates and generalized points-to graphs. Its default
configuration is flow-sensitive, field-sensitive, and fully
context-sensitive. Procedure summaries retain only control flow required by
data dependences.

Implementation
--------------

**Headers**: ``include/Alias/InclusionBased/GPG/``

**Sources**: ``lib/Alias/InclusionBased/GPG/``

**Tool**: ``lotus-alias-gpg``

The implementation includes:

- TS and SS GPU composition and maximal GPU reduction;
- boundary definitions and upwards-exposed locations;
- reaching-GPU analyses with and without barrier blocking;
- queued compositions, strong and weak updates, and dead-GPU elimination;
- coherent GPB coalescing and control-flow minimization;
- allocation-site heap abstraction and k-limited field-sensitive indirection
  lists;
- bottom-up call inlining and fixed-point refinement of recursive SCCs;
- delayed function-pointer resolution and call-graph refinement; and
- statement-specific points-to information plus per-procedure mod/ref
  summaries.

LLVM integration
----------------

The frontend translates LLVM SSA and CFG constructs directly. LLVM globals,
allocas, allocation calls, formals, returns, and functions receive stable
abstract locations. GEP indices become field steps; arrays are index
insensitive by default. SSA definitions remain immutable during blocking,
which has the same role as the reference implementation's def-use-chain
resolution.

Usage
-----

.. code-block:: bash

   ./build/bin/lotus-alias-gpg --mode=fscs \
     --print-pts --print-call-graph --print-modref input.bc

The supported modes are:

- ``fscs``: flow- and context-sensitive (default);
- ``fics``: flow-insensitive and context-sensitive; and
- ``fici``: flow- and context-insensitive.

Use ``--heap-k=N`` to select the heap indirection-list bound (default 3).
``--array-index-sensitive`` optionally distinguishes constant array indices.

The analysis is also available as ``AAConfig::GPG()`` through
``AliasAnalysisWrapper`` and as ``--cg-type=gpg`` in
``lotus-alias-call-graph``.
