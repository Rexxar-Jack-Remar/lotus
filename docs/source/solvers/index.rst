Solvers
=======

This section documents the solver frameworks, constraint solving backends, and
SMT-based model checking components used throughout Lotus.

The available backends include BDD (CUDD), SMT (Z3-based), weighted pushdown
systems (WPDS), string constraint solving (Stingx), fixed-point equation
solving (FPsolve), and experimental solver tooling (SLOT, STAUB, EGraph,
SymAbs, SMTSampler).

.. toctree::
   :maxdepth: 2

   cudd
   smt
   wpds
   smtsampler
   symabs
   slot
   staub
   stingx
   egraphs_simp
   fpsolve
