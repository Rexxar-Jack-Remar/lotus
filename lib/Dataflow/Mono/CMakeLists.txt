# Mono-based dataflow analysis implementation

set(MONO_SOURCES
  Analyses/Intraprocedural/IntraMonoConstantPropagation.cpp
  Analyses/Intraprocedural/IntraMonoFullConstantPropagation.cpp
  Analyses/Intraprocedural/IntraMonoSolverTest.cpp
  Analyses/Intraprocedural/IntraMonoUninitVariables.cpp
  Analyses/Intraprocedural/LiveVariablesAnalysis.cpp
  Analyses/Intraprocedural/ReachableAnalysis.cpp
  Analyses/Interprocedural/InterMonoTaintAnalysis.cpp
  Analyses/Interprocedural/InterMonoConstantPropagation.cpp
  Analyses/Interprocedural/InterMonoFullConstantPropagation.cpp
  Analyses/Interprocedural/InterMonoSolverTest.cpp
)

add_library(MONODataFlow STATIC ${MONO_SOURCES})

  
