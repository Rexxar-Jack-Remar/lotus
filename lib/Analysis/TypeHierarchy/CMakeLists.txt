set(CanaryTypeHierarchy_SOURCES
  TypeHierarchyAnalysis.cpp
  DIBasedTypeHierarchy.cpp
  DIBasedTypeHierarchyData.cpp
  LLVMVFTable.cpp
  LLVMVFTableData.cpp
  LLVMVFTableProvider.cpp
  VirtualCallUtils.cpp
  AllocatedTypes.cpp
  CallGraphAnalysisType.cpp
  CallGraph.cpp
  CallGraphBuilder.cpp
  Resolver.cpp
  CHA/CHAResolver.cpp
  RTA/RTAResolver.cpp
  VTA/VTAResolver.cpp
  VTA/TypeAssignmentGraph.cpp
  VTA/TypePropagator.cpp
  OTF/OTFResolver.cpp
)
add_library(CanaryTypeHierarchy STATIC ${CanaryTypeHierarchy_SOURCES})
target_link_libraries(CanaryTypeHierarchy CanaryLLVMUtils CanaryCFG)
