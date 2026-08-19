Aria: Grammar-Driven CFL Reachability
=====================================

``include/CFL/Aria/`` and ``lib/CFL/Aria/`` provide a compact CFL reachability
toolkit centered on explicit grammars, labeled graphs, and reusable solver
front-ends.

**Location**: ``include/CFL/Aria/``, ``lib/CFL/Aria/``

**Main components**:

- ``Grammar`` parses grammars from text or files, tracks nullable symbols, and
  builds unary and binary production indices for fast lookup.
- ``CNFGrammar`` loads grammars and applies the STBDU normalization pipeline,
  including start, terminal, binarization, epsilon-removal, and unit-production
  transforms.
- ``LabeledGraph`` stores the reachability instance, supports text and DOT
  inputs, and exposes matrix/PAG-style loading modes plus label-indexed edge
  queries.
- ``CFLSolver`` computes classical CFL reachability closure and reports
  iteration and edge-growth statistics.
- ``SCSolver`` builds a set-constraint system over the same graph and grammar
  inputs and returns constraint-system statistics.
- ``SVFPort`` bridges Aria to Lotus IR analyses by encoding alias-constraint
  graphs and SVFG instances into labeled graphs and grammars.

**Integration points**:

- ``AliasClient`` supports may-alias and points-to style queries from encoded
  PAG or PEG constraint graphs.
- ``ValueFlowClient`` answers reachability queries over encoded SVFG value-flow
  graphs.
- ``lib/CFL/Aria/CMakeLists.txt`` builds the ``CanaryAriaCFL`` library target.
- ``tests/unit/CFL/AriaCFLTest.cpp`` covers grammar parsing, graph loading,
  classical solving, set-constraint solving, and CNF normalization.

See also :doc:`cfl_components`.
