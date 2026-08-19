CFL Tools
=========

This page documents the CFL-related tools under ``tools/cfl/``. For the
underlying theory and components, see :doc:`../../cfl/cfl_components`.

Overview
--------

Context-Free Language (CFL) reachability extends graph reachability with
context-free grammars for precise interprocedural analysis. CFL reachability
enables analysis of complex program properties using grammar-based constraints.

**Location**: ``tools/cfl/``

**Tools**: CSR (indexed CFL reachability)

CSR: Context-Sensitive Reachability
-----------------------------------

Indexing-based context-sensitive reachability engine for large graphs.

**Binary**: ``csr``  
**Location**: ``tools/cfl/csr/csr.cpp``

CSR operates on graph files (not LLVM bitcode directly) and answers reachability
queries with different indexing strategies (GRAIL, PathTree, or combined).

**Basic Usage**:

.. code-block:: bash

   ./build/bin/csr [options] graph_file

**Common Options** (see ``tools/cfl/csr/README.md`` for full list):

- ``-m <method>`` – Indexing method:

  - ``pathtree`` – PathTree indexing
  - ``grail`` – GRAIL labeling
  - ``pathtree+grail`` – Combined approach

- ``-t`` – Evaluate transitive closure
- ``-r`` – Evaluate tabulation algorithm
- ``-p`` – Evaluate parallel tabulation algorithm
- ``-j <N>`` – Number of threads for parallel tabulation (0 = auto)
- ``-g <file>`` – Generate queries and save to file
- ``-q <file>`` – Load queries from file

**Examples**:

.. code-block:: bash

   # GRAIL-based reachability
   ./build/bin/csr input.graph

   # PathTree indexing
   ./build/bin/csr -m pathtree input.graph

   # Parallel tabulation with 4 threads
   ./build/bin/csr -p -j 4 input.graph
