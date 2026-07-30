CSIndex: Indexed Context-Sensitive CFL Reachability
===================================================

``include/CFL/CSIndex/`` and ``lib/CFL/CSIndex/`` implement the indexed
context-sensitive reachability engine used by the ``csr`` tool.

**Location**: ``include/CFL/CSIndex/``, ``lib/CFL/CSIndex/``

**Main components**:

- ``Graph`` and ``ReachBackbone`` build the indexed graph representation.
- ``Query`` and ``PathtreeQuery`` answer reachability queries.
- ``Tabulation`` and ``ParallelTabulation`` provide exact query engines.
- ``Grail`` adds pruning through reachability labeling.

**Use cases**:

- Fast context-sensitive CFL reachability on large graph inputs.
- Comparing indexing strategies such as GRAIL and PathTree.
- Backing the ``tools/cfl/csr`` command-line front-end.

Query workflow
--------------

Build the indexed graph and reachability backbone once, then issue ``Query``
or ``PathtreeQuery`` requests against that index.  This up-front work is what
makes the subsystem useful when many context-sensitive questions target the
same graph.  ``Grail`` is a pruning aid, so clients should retain an exact
tabulation engine for answers that must not rely on an approximation.

See also :doc:`cfl_components` and :doc:`../tools/cfl/index`.
