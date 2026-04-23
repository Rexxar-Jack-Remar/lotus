Algebraic Program Analysis (APA)
=============================

``APA`` is Lotus's elimination-based dataflow analysis (or, "algebraic program analysis") framework.

**Headers**: ``include/Dataflow/APA/``

**Implementation**: ``lib/Dataflow/APA/``

Overview
--------

APA expresses classical dataflow problems as algebraic program analysis and solves
them with elimination-style algorithms. The framework currently provides LLVM
clients for standard intraprocedural analyses and reusable solver backends.

Main components
---------------

- ``Core/`` defines generic problem, result, and option abstractions.
- ``Solver/`` contains solver implementations such as state elimination,
  ADT-simple, and ADT-delayed solvers.
- ``Analyses/LLVM/Intra/`` provides ready-made intraprocedural analyses:

  - available expressions
  - constant propagation
  - live variables
  - non-null
  - reachability
  - reaching definitions
  - uninitialized variables
  - very busy expressions

- ``Passes/EliminationPasses`` exposes LLVM-pass integration.

Typical use cases
-----------------

- Compare elimination-based solving with Mono or IFDS engines.
- Prototype intraprocedural dataflow problems over LLVM IR.
- Drive differential-testing workflows for solver validation.

See also
--------

- See :doc:`mono`, :doc:`ifds_ide`, and :doc:`npa` for related engines.
- See :doc:`../tools/dataflow/index` for the testing front-ends.
