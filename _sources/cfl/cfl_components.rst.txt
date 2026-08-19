CFL Reachability Components
===========================

Advanced CFL reachability algorithms and graph analysis frameworks.

Aria
----

Grammar-driven CFL reachability utilities, solver backends, and SVF adapters.

**Location**: ``include/CFL/Aria/``, ``lib/CFL/Aria/``

**Features**:
* Grammar parsing and CNF/STBDU normalization helpers
* Labeled graph construction for text, DOT, PAG, and PEG-style encodings
* Classical and set-constraint solvers for reachability closure
* Adapters for alias and value-flow problems built on SVF structures

CSIndex (Context-Sensitive Indexing)
-----------------------------------

Context-sensitive indexing for CFL reachability.

**Location**: ``lib/CFL/CSIndex/``

**Features**: Context-aware indexing algorithms for efficient CFL queries.

**Components**:
* Context-sensitive graph indexing
* Reachability query optimization
* Memory-efficient representations


InterDyckGraphReduce
-------------------

Interprocedural Dyck graph reduction algorithms.

**Location**: ``lib/CFL/InterDyckGraphReduce/``

**Features**: Interprocedural analysis with graph reduction techniques for Dyck languages.

MutualRefinement
---------------

Mutual refinement algorithms for CFL analysis.

**Location**: ``lib/CFL/MutualRefinement/``

**Features**: Bidirectional refinement techniques for improving analysis precision.

See also:

- :doc:`aria`
- :doc:`csindex`
- :doc:`inter_dyck_graph_reduce`
- :doc:`mutual_refinement`
