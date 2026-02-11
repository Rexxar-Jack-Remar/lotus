# SVFG Library
# Production-ready Sparse Value-Flow Graph with AserPTA integration

# Sources
set(SVFG_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/SVFG.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/SVFGBuilder.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/SVFGBuilderNodes.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/SVFGBuilderEdges.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/SVFGBuilderMemorySSA.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/SVFGSerializer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/SVFGOPT.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/SVFGStats.cpp
)

# Create library
add_library(SVFG STATIC ${SVFG_SOURCES} ${SVFG_HEADERS})

# Link dependencies
# AserPTA is the default pointer analysis (lib/Alias/AserPTA)
target_link_libraries(SVFG PUBLIC
    CanaryICFG
    AserPTA
    LLVMCore
    LLVMSupport
)

