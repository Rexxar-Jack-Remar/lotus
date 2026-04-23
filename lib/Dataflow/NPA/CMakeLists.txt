# NPA sources
set(NPA_SOURCES
  Domains/AffineRelationDomain.cpp
  Domains/BitVectorDomain.cpp
  Domains/GenKillDomain.cpp
  Domains/PredicateRelationDomain.cpp
  Domains/TaintTransferDomain.cpp

  Analyses/BitVectorSolver.cpp
  Analyses/Intra/LiveVariables.cpp
  Analyses/Inter/InterAffineEqualities.cpp
  Analyses/Inter/InterConstantPropagation.cpp
  Analyses/Inter/InterIntervalAnalysis.cpp
  Analyses/Inter/InterLiveVariables.cpp
  Analyses/Inter/InterNullability.cpp
  Analyses/Intra/ReachingDefinitions.cpp
  Analyses/Intra/ReachableBlocks.cpp
  Analyses/Inter/InterMaybeUninitialized.cpp
  Analyses/Inter/InterReachingDefinitions.cpp
  Analyses/Inter/InterTaint.cpp
)

add_library(NPADataFlow STATIC ${NPA_SOURCES})
target_link_libraries(NPADataFlow PUBLIC CanaryCUDD CanaryParallel)
