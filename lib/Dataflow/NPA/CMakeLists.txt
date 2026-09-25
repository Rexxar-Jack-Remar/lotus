# Header-only framework components live under include/Dataflow/NPA/.
# NPADataFlow contains compiled domains, LLVM integration, and analyses.
set(NPA_SOURCES
  Domains/BitSetDomain.cpp
  Domains/ConstantPropagationDomain.cpp
  Domains/IntervalDomain.cpp
  Domains/PredicateRelationDomain.cpp
  Domains/GenKillDomain.cpp
  Domains/TaintDomain.cpp

  LLVM/BitVectorSolver.cpp
  Analyses/Intra/ConstantPropagation.cpp
  Analyses/Intra/Interval.cpp
  Analyses/Intra/LiveVariables.cpp
  Analyses/Intra/MaybeUninitialized.cpp
  Analyses/Intra/Nullability.cpp
  Analyses/Intra/ReachableBlocks.cpp
  Analyses/Intra/ReachingDefinitions.cpp
  Analyses/Intra/Taint.cpp
  Analyses/Inter/ConstantPropagation.cpp
  Analyses/Inter/Interval.cpp
  Analyses/Inter/LiveVariables.cpp
  Analyses/Inter/MaybeUninitialized.cpp
  Analyses/Inter/Nullability.cpp
  Analyses/Inter/ReachableBlocks.cpp
  Analyses/Inter/ReachingDefinitions.cpp
  Analyses/Inter/Taint.cpp
)

add_library(NPADataFlow STATIC ${NPA_SOURCES})
target_link_libraries(NPADataFlow PUBLIC CanaryCUDD)

add_library(NPADataFlowDrivers STATIC
  Tooling/NPALLVMDriver.cpp
  Tooling/NPABooleanDriver.cpp)
