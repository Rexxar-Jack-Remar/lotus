# Program Dependency Graph (PDG) CMake configuration
#
# This file configures the build for the PDG module, which implements
# a comprehensive program dependency graph for LLVM-based analysis.
# 

add_library(CanaryPDG STATIC
  Core/CallWrapper.cpp
  Core/ControlDependencyGraph.cpp
  Core/DataDependencyGraph.cpp
  Core/FunctionWrapper.cpp
  Core/Graph.cpp
  Core/PDGCallGraph.cpp
  Core/PDGNode.cpp
  Core/ProgramDependencyGraph.cpp
  Core/Tree.cpp
  Analysis/ContextSensitiveSlicing.cpp
  Analysis/CypherQuery.cpp
  Analysis/DataFlowQuery.cpp
  Analysis/DependenceQuery.cpp
  Analysis/MotionLegality.cpp
  Analysis/PDGDiff.cpp
  Analysis/SchedulingQuery.cpp
  Analysis/Slicing.cpp
  Analysis/ThinSlicing.cpp
  Support/DebugInfoUtils.cpp
  Support/GraphWriter.cpp
  Support/PDGUtils.cpp
) 