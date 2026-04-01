Path Expression Algorithms
==========================

``include/Utils/Algorithms/`` contains algorithmic helpers that do not fit into
the general-purpose container layer.

**Current focus**: ``PathExpressions/``

- ``PathExpressionComputer`` computes path summaries over labeled graphs.
- ``Regex`` stores the resulting path-expression representation.
- ``LabeledGraph`` is the graph interface used by the algorithm.
- ``RegexToTgf`` and ``RegexToCompactTgf`` export debugging views.

This code is useful when analyses need compact descriptions of all paths between
two nodes rather than explicit path enumeration.

See also :doc:`utilities`.
