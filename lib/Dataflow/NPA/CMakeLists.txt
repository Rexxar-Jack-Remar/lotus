# NPA sources
set(NPA_SOURCES
  Domains/BitVectorDomain.cpp
  Domains/GenKillDomain.cpp
  Domains/TaintTransferDomain.cpp

  Analyses/BitVectorSolver.cpp
  Analyses/Intraprocedural/ReachingDefinitions.cpp
  Analyses/Intraprocedural/ReachableBlocks.cpp
  Analyses/Interprocedural/InterproceduralRD.cpp
  Analyses/Interprocedural/InterproceduralTaint.cpp
)

add_library(NPADataFlow STATIC ${NPA_SOURCES})
