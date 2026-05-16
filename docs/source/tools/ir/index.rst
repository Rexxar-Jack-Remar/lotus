PDG Query – Program Dependence Graph Queries
==============================================

Interactive and batch query engine for the Program Dependence Graph (PDG).

**Binary**: ``lotus-ir-pdg-query``  
**Location**: ``tools/ir/lotus-ir-pdg-query.cpp``

**Usage**:

.. code-block:: bash

   # Interactive mode
   ./build/bin/lotus-ir-pdg-query -i program.bc

   # Single query
   ./build/bin/lotus-ir-pdg-query -q "MATCH (n:FUNC_ENTRY) WHERE n.name = 'main' RETURN n" program.bc

   # Batch queries from file
   ./build/bin/lotus-ir-pdg-query -f queries.txt program.bc

Key features:

- Forward/backward slicing
- Property-based slicing via ``--property-file``
- Information flow queries
- Security policy checks
- Subgraph export (DOT)

See :doc:`../../user_guide/pdg_query_language` for the language reference and
:doc:`examples` for the in-repo query cookbook.

.. toctree::
   :maxdepth: 1

   examples
