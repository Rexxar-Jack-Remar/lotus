# Sea-dsa sanity checks option (localized to seadsa)
option(SEADSA_ENABLE_SANITY_CHECKS "Enable Sea-dsa sanity checks." OFF)
if(SEADSA_ENABLE_SANITY_CHECKS)
  message(STATUS "Sea-dsa: compiling with sanity checks")
else()
  message(STATUS "Sea-dsa: compiling without sanity checks")
endif()

# Configure seadsa config header
configure_file(${CMAKE_SOURCE_DIR}/include/Alias/seadsa/config.h.cmake
  ${CMAKE_BINARY_DIR}/include/Alias/seadsa/config.h)

# Create seadsa library
add_library(SeaDsaAnalysis
  Graph.cc
  FieldType.cc
  DsaLocal.cc
  DsaGlobal.cc
  DsaCallSite.cc
  Cloner.cc
  DsaInfo.cc
  DsaStats.cc
  Mapper.cc
  DsaBottomUp.cc
  DsaTopDown.cc
  DsaAnalysis.cc
  DsaPrinter.cc
  CallGraphWrapper.cc
  TypeUtils.cc
  Debug.cc
  NameValues.cc
  RemovePtrToInt.cc
  AllocWrapInfo.cc
  AllocSiteInfo.cc
  DsaAllocSite.cc
  DsaCompleteCallGraph.cc
  DsaCallGraphPrinter.cc
  DsaColor.cc
  ShadowMem.cc
  SeaMemorySSA.cc
  SeaDsaAliasAnalysis.cc
  DsaLibFuncInfo.cc
  InitializePasses.cc
)

set_target_properties(SeaDsaAnalysis PROPERTIES
  CXX_STANDARD 17
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF
)
target_compile_features(SeaDsaAnalysis PUBLIC cxx_std_17)

# Link against LLVM libraries
target_link_libraries(SeaDsaAnalysis PUBLIC
  LLVMIRReader
  LLVMTransformUtils
)

# Set compile definitions
if (SEADSA_ENABLE_SANITY_CHECKS)
  target_compile_definitions(SeaDsaAnalysis PRIVATE SEADSA_SANITY_CHECKS)
endif()

# Install library
install(TARGETS SeaDsaAnalysis
  LIBRARY DESTINATION lib
  ARCHIVE DESTINATION lib
)
