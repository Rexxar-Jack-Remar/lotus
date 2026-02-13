# Elimination-based (state-elimination) intraprocedural dataflow solver.
#
# The solver itself is template-based and lives in include/Dataflow/Elimination/.
# This target exists mainly for uniform build integration and compilation checks.

set(ELIMINATION_SOURCES
  Elimination.cpp
  EliminationPasses.cpp
  Analyses/Intra/IntraReachable.cpp
  Analyses/Intra/IntraConstantPropagation.cpp
  Analyses/Intra/IntraReachingDefinitions.cpp
  Analyses/Intra/IntraAvailableExpressions.cpp
  Analyses/Intra/IntraUninitVariables.cpp
  Analyses/Intra/IntraLiveVariables.cpp
  Analyses/Intra/IntraVeryBusyExpressions.cpp
  Analyses/Intra/IntraNonNull.cpp
)

add_library(EliminationDataFlow STATIC ${ELIMINATION_SOURCES})
