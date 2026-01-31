# Mono-based dataflow analysis implementation

set(MONO_SOURCES
  Solver/CallStringInterProceduralDataFlow.cpp

  Analyses/Intraprocedural/IntraMonoConstantPropagation.cpp
  Analyses/Intraprocedural/IntraMonoUninitVariables.cpp
  Analyses/Intraprocedural/LiveVariablesAnalysis.cpp
  Analyses/Intraprocedural/ReachableAnalysis.cpp
  Analyses/Interprocedural/InterMonoTaintAnalysis.cpp
)

add_library(MONODataFlow STATIC ${MONO_SOURCES})

  
