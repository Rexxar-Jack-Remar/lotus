CERT-CFL: Cardinality-Certified Exact Solving
=============================================

CERT-CFL is an exact all-symbol CFL-reachability solver proposed by the Lotus
team. It uses universal-degree certificates, symbolic nullable identity, a
finite threshold domain, and partition refinement to promote dense relation
blocks to exactness without enumerating every pair. No external paper is
associated with the algorithm; see the source headers for the precise
promotion rules.

Problem and key idea
--------------------

The kernel works on normalized epsilon, unary, and binary productions plus
seed facts (terminal or nonterminal edges). Nodes and symbols are dense,
zero-based IDs. Instead of a concrete triple worklist, CERT-CFL partitions
the nodes into blocks and maintains, for every symbol and block pair, a
**tile** that summarizes the relation between the two blocks:

``may``
   The tile contains at least one derivation that uses a seed edge.

``out`` / ``in``
   Universal minimum outgoing/incoming degree bounds over the block, i.e.
   lower bounds valid for every member node, not averages.

A tile is promoted to exact (``FULL``) once its universal degree bounds prove
that it contains every node pair of the target block. The promotion condition
for ``A -> B C`` with a shared pivot block ``K`` is the strict inequality

.. code-block:: cpp

   const bool overlap = left.out > middle_size - right.in;

i.e. ``left.out + right.in > |K|``, written to avoid unsigned overflow.
Multiple proofs combine using maximum, not addition. Nullable identity
participates in these lower bounds but never sets the ``may`` bits, which
permits exact ``IDENTITY`` tiles without refining them into singleton
diagonals.

Refinement
----------

The default finite threshold domain is ``{0, 1, floor(t/2)+1, t}`` with
rounding downward. Blocks that cannot be certified at the current granularity
are bisected, and saturation continues on the finer partition. Balanced block
bisection eventually reaches singleton resolution, which is always exact.
Recursive transfer uses copies of premise tiles so an output tile may alias an
input tile.

Resource limits and sparse fallback
-----------------------------------

``Options`` bounds the dense work:

``max_tiles``
   A count of dense tile records (24 bytes each on 64-bit hosts), not a
   process-memory cap.

``max_levels`` / ``max_dense_joins``
   Bound refinement depth and cumulative dense joins; zero means unlimited.

``on_limit``
   ``SparseFallback`` (default) switches to an independent exact sparse
   worklist closure; ``Throw`` prohibits the fallback.

``max_sparse_facts``
   Caps concrete facts (including identities) while sparse closure runs;
   zero means unlimited.

The sparse fallback never returns an unresolved abstract answer as exact. A
configured hard limit or allocation failure propagates as an exception; no
partial result is published. ``observed = nullopt`` resolves all symbols; an
empty observed vector means no required outputs.

Querying results
----------------

``Result`` is an immutable, shared-storage snapshot.

``answer(symbol, u, v)``
   Returns ``std::optional<bool>``; ``nullopt`` is **unknown**, never false.

``contains(symbol, u, v)``
   Throws if the requested pair remains unresolved.

Exact whole-symbol traversal and counts throw ``logic_error`` for an
unresolved symbol, while individual pairs may still be answered definitively.
Counts for resolved symbols are cached; the off-diagonal union counter handles
overlap across symbols without expanding ``FULL`` tiles into pair sets.

Lotus implementation
--------------------

Public API
   ``include/CFL/Classical/Solvers/Engines/CERT/CertCFL.h`` (dependency-free
   kernel) and ``CertCFLEngine.h`` (``Relation`` adapter).

Algorithm
   ``lib/CFL/Classical/Solvers/Engines/CERT/CertCFL.cpp`` and
   ``CertCFLEngine.cpp``.

Integration
   ``SolverBackend::CertCFL`` and ``--solver cert-cfl`` in the classical,
   alias, and value-flow drivers.

The adapter resolves **all grammar symbols** because ``Relation`` supports
arbitrary-label membership, visitors, and total counts; setting
``options.cert_cfl.observed`` is rejected. ``unidirectional=true`` is also
rejected: CERT-CFL does not implement restricted POCR Insert/Follow
evaluation. Seed-only symbols (no unary/binary defining rule) are kept as
exact sparse inputs plus symbolic identity, so they do not force singleton
refinement.

Updates follow the completed-snapshot contract shared with the
endpoint-quotient engine: ``add`` buffers monotone seed insertions, queries
see the last completed snapshot (empty before the first solve), and a failed
solve preserves the old snapshot and all pending inputs. Updated solves
rebuild from the accumulated inputs; they are not delta-incremental. Mutation
is single-threaded; do not update or solve from a traversal callback.

Validation
----------

The native ``CertCFLEngineTest`` compares complete labeled relations against
the ``SparseSet`` reference backend, exercises monotone incremental rounds and
nullable nodes, forces the sparse-fallback budget path, checks the
off-diagonal Count-symbol union, and confirms the rejection of unsupported
modes and external graph mutation. The standalone kernel was additionally
differential-tested against an independent Boolean fixed-point reference.