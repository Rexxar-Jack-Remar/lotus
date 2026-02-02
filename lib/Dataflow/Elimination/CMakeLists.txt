# Elimination-based (state-elimination) intraprocedural dataflow solver.
#
# The solver itself is template-based and lives in include/Dataflow/Elimination/.
# This target exists mainly for uniform build integration and compilation checks.

set(ELIMINATION_SOURCES
  Elimination.cpp
)

add_library(EliminationDataFlow STATIC ${ELIMINATION_SOURCES})

