# Elimination-based (state-elimination) intraprocedural dataflow solver.
#
# The solver itself is template-based and lives in include/Dataflow/Elimination/.
# This target exists mainly for uniform build integration and compilation checks.

set(ELIMINATION_SOURCES
  Elimination.cpp
  EliminationPasses.cpp
  Analyses/Intraprocedural/EliminationReachable.cpp
  Analyses/Intraprocedural/EliminationConstantPropagation.cpp
  Analyses/Intraprocedural/EliminationReachingDefinitions.cpp
  Analyses/Intraprocedural/EliminationAvailableExpressions.cpp
  Analyses/Intraprocedural/EliminationUninitVariables.cpp
  Analyses/Intraprocedural/EliminationLiveVariables.cpp
  Analyses/Intraprocedural/EliminationVeryBusyExpressions.cpp
  Analyses/Intraprocedural/EliminationNonNull.cpp
)

add_library(EliminationDataFlow STATIC ${ELIMINATION_SOURCES})
