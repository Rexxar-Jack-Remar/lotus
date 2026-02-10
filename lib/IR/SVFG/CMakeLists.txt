# SVFG Library
# Production-ready Sparse Value-Flow Graph with AserPTA integration

# Headers - located in include/IR/SVFG/
set(SVFG_HEADERS
    ${CMAKE_SOURCE_DIR}/include/IR/SVFG/SVFGBase.h
    ${CMAKE_SOURCE_DIR}/include/IR/SVFG/SVFG.h
    ${CMAKE_SOURCE_DIR}/include/IR/SVFG/SVFGNode.h
    ${CMAKE_SOURCE_DIR}/include/IR/SVFG/SVFGEdge.h
    ${CMAKE_SOURCE_DIR}/include/IR/SVFG/SVFGBuilder.h
    ${CMAKE_SOURCE_DIR}/include/IR/SVFG/SVFGSerializer.h
    ${CMAKE_SOURCE_DIR}/include/IR/SVFG/SVFGOPT.h
    ${CMAKE_SOURCE_DIR}/include/IR/SVFG/SVFGStats.h
    ${CMAKE_SOURCE_DIR}/include/IR/SVFG/GraphTraits.h
)

# Sources
set(SVFG_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/SVFG.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/SVFGBuilder.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/SVFGSerializer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/SVFGOPT.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/SVFGStats.cpp
)

# Create library
add_library(SVFG STATIC ${SVFG_SOURCES} ${SVFG_HEADERS})

# Include directories
target_include_directories(SVFG PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)

# Link dependencies
# AserPTA is the default pointer analysis (lib/Alias/AserPTA)
target_link_libraries(SVFG PUBLIC
    ICFG
    AserPTA
    LLVMCore
    LLVMSupport
)

# Set C++ standard
target_compile_features(SVFG PRIVATE cxx_std_14)

# Install headers
install(FILES ${SVFG_HEADERS}
    DESTINATION include/IR/SVFG
)
