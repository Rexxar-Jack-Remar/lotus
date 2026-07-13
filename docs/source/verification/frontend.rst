Verification Frontend
====================

BooleanProgram
--------------

**Location**: ``lib/Verification/Frontend/BooleanProgram/``

**Status**: Internal/Experimental — not exposed via any production tool.

A parser+lowerer for a Boolean/predicate program specification language
(Bebop/SATABS-style). Reads a textual format where programs consist of
predicates with procedures, control-flow statements, and BooleanExpr terms.
Lowers parsed programs to the NPA dataflow framework via
``PredicateProgramLowering``.

**Components**:
- ``BooleanProgramParser`` — recursive-descent parser (hand-written)
- ``BooleanProgram`` — AST data structures (Procedure, Statement, BooleanExpr, etc.)
- ``PredicateProgramLowering`` — lowers to NPA ``PredicateRelation``-based CFG

Only linked by unit tests (``boolean_program_frontend_test``).
