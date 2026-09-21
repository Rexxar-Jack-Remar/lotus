# Optional cclyzer++ (Datalog-based pointer analysis for LLVM IR).
# When enabled, builds the CclyzerAA wrapper library that uses cclyzerpp's
# analysis. Does not integrate into AliasAnalysisWrapper.

option(LOTUS_ENABLE_CCLYZER "Enable optional cclyzer++ Datalog-based alias analysis backend" OFF)
if(DEFINED LOTUS_USE_CCLYZER)
  set(LOTUS_ENABLE_CCLYZER ${LOTUS_USE_CCLYZER} CACHE BOOL
      "Enable optional cclyzer++ Datalog-based alias analysis backend" FORCE)
endif()

set(CCLYZERPP_ROOT "" CACHE PATH "Path to cclyzer++ source tree (defaults to third-party/cclyzerpp)")

if(NOT LOTUS_ENABLE_CCLYZER)
  return()
endif()

if(NOT CCLYZERPP_ROOT OR NOT EXISTS "${CCLYZERPP_ROOT}/CMakeLists.txt")
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third-party/cclyzerpp/CMakeLists.txt")
    set(CCLYZERPP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third-party/cclyzerpp" CACHE PATH "Path to cclyzer++ source tree" FORCE)
  else()
    message(WARNING
      "LOTUS_ENABLE_CCLYZER is ON but CCLYZERPP_ROOT is missing or invalid. "
      "Set -DCCLYZERPP_ROOT=/path/to/cclyzerpp to enable CclyzerAA.")
    set(LOTUS_ENABLE_CCLYZER OFF PARENT_SCOPE)
    return()
  endif()
endif()

# Soufflé is required by cclyzerpp (build-time: compile .dl -> .cpp; runtime: headers).
find_program(SOUFFLE_BIN NAMES souffle souffle-compile DOC "Soufflé compiler")
find_path(SOUFFLE_INCLUDE NAMES souffle/SouffleInterface.h
  HINTS /usr/include /usr/local/include
  DOC "Soufflé include directory")

if(NOT SOUFFLE_BIN OR NOT SOUFFLE_INCLUDE)
  message(WARNING
    "cclyzerpp requires Soufflé. Set SOUFFLE_BIN and SOUFFLE_INCLUDE, "
    "or install souffle (e.g. package manager). Disabling CclyzerAA.")
  set(LOTUS_ENABLE_CCLYZER OFF PARENT_SCOPE)
  return()
endif()

message(STATUS "CclyzerAA: using cclyzer++ at ${CCLYZERPP_ROOT}")
message(STATUS "CclyzerAA: Soufflé binary ${SOUFFLE_BIN}, include ${SOUFFLE_INCLUDE}")

# cclyzerpp expects LLVM_MAJOR_VERSION (default 14). Lotus uses LLVM 14.
if(NOT DEFINED LLVM_MAJOR_VERSION)
  set(LLVM_MAJOR_VERSION 14)
endif()

# Build cclyzerpp in-tree (defines PAPass, SoufflePA, factgen-exe).
add_subdirectory(${CCLYZERPP_ROOT} ${CMAKE_BINARY_DIR}/cclyzerpp)

# CclyzerAA wrapper will be added from lib/Alias/CMakeLists.txt when this succeeds.
set(LOTUS_CCLYZERPP_AVAILABLE ON)
