# Find and configure LLVM
# Preferred workflow:
#   1. Try to find a system-installed LLVM (14.x), e.g. via:
#        - system package managers (apt, Homebrew, etc.)
#        - CMake variable LLVM_DIR
#        - llvm-config on PATH
#   2. If that fails, require the user to specify LLVM_BUILD_PATH pointing to
#      the directory that contains LLVMConfig.cmake (for a local or custom build).
#
# This keeps common setups simple (no extra flags) and still supports custom LLVM builds.

# First, try to find a system LLVM without requiring an explicit path.
find_package(LLVM QUIET CONFIG)

# Validate system LLVM version (require 14.x)
if(LLVM_FOUND)
  if(NOT (LLVM_PACKAGE_VERSION VERSION_GREATER_EQUAL "14.0.0" AND LLVM_PACKAGE_VERSION VERSION_LESS "15.0.0"))
    # Version not satisfied; clear result to trigger fallback/diagnostic
    unset(LLVM_FOUND CACHE)
  endif()
endif()

# If not found or version not satisfied, try an explicit path if provided;
# otherwise, instruct the user to install or set LLVM_BUILD_PATH.
if(NOT LLVM_FOUND)
  if(LLVM_BUILD_PATH)
    # Search only under the provided path
    find_package(LLVM REQUIRED CONFIG PATHS "${LLVM_BUILD_PATH}" NO_DEFAULT_PATH)
    if(NOT (LLVM_PACKAGE_VERSION VERSION_GREATER_EQUAL "14.0.0" AND LLVM_PACKAGE_VERSION VERSION_LESS "15.0.0"))
      message(FATAL_ERROR
        "Found LLVM at LLVM_BUILD_PATH but version is ${LLVM_PACKAGE_VERSION}. "
        "This project requires LLVM 14.x")
    endif()
  else()
    message(FATAL_ERROR
      "LLVM 14.x is required but not found on the system path. "
      "Please install LLVM 14.x, or re-run CMake with "
      "-DLLVM_BUILD_PATH=/path/to/llvm/lib/cmake/llvm")
  endif()
endif()

message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")

# Derive a stable LLVM tools directory for tests and helper commands. Prefer
# the path exported by LLVMConfig.cmake; fall back to common layouts relative
# to llvm-config or LLVM_DIR when needed.
set(LOTUS_LLVM_TOOLS_BINARY_DIR "")
if(DEFINED LLVM_TOOLS_BINARY_DIR AND EXISTS "${LLVM_TOOLS_BINARY_DIR}")
  set(LOTUS_LLVM_TOOLS_BINARY_DIR "${LLVM_TOOLS_BINARY_DIR}")
elseif(DEFINED LLVM_TOOLS_BINARY_DIR AND EXISTS "${LLVM_TOOLS_BINARY_DIR}/opt")
  set(LOTUS_LLVM_TOOLS_BINARY_DIR "${LLVM_TOOLS_BINARY_DIR}")
elseif(DEFINED LLVM_TOOLS_BINARY_DIR AND EXISTS "${LLVM_TOOLS_BINARY_DIR}/clang++")
  set(LOTUS_LLVM_TOOLS_BINARY_DIR "${LLVM_TOOLS_BINARY_DIR}")
elseif(DEFINED LLVM_TOOLS_BINARY_DIR AND EXISTS "${LLVM_TOOLS_BINARY_DIR}/clang++-14")
  set(LOTUS_LLVM_TOOLS_BINARY_DIR "${LLVM_TOOLS_BINARY_DIR}")
elseif(DEFINED LLVM_TOOLS_BINARY_DIR AND EXISTS "${LLVM_TOOLS_BINARY_DIR}/opt-14")
  set(LOTUS_LLVM_TOOLS_BINARY_DIR "${LLVM_TOOLS_BINARY_DIR}")
elseif(DEFINED LLVM_TOOLS_BINARY_DIR AND LLVM_TOOLS_BINARY_DIR)
  set(LOTUS_LLVM_TOOLS_BINARY_DIR "${LLVM_TOOLS_BINARY_DIR}")
elseif(DEFINED LLVM_DIR AND LLVM_DIR)
  get_filename_component(_lotus_llvm_prefix "${LLVM_DIR}/../../.." ABSOLUTE)
  if(EXISTS "${_lotus_llvm_prefix}/bin")
    set(LOTUS_LLVM_TOOLS_BINARY_DIR "${_lotus_llvm_prefix}/bin")
  endif()
endif()

if(LOTUS_LLVM_TOOLS_BINARY_DIR)
  message(STATUS "Using LLVM tools from ${LOTUS_LLVM_TOOLS_BINARY_DIR}")
endif()

# Include LLVM CMake modules to make functions # like add_llvm_library available (if you want to use it)
# include(AddLLVM)

# Set LLVM version-specific definitions (only 14.x supported)
add_definitions(-DLLVM14)

# Configure LLVM
include_directories(${LLVM_INCLUDE_DIRS}
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}/include/Verification
  ${CMAKE_CURRENT_SOURCE_DIR}/third-party
  ${CMAKE_BINARY_DIR}/include
  ${CMAKE_BINARY_DIR}/include/Verification
  )
add_definitions(${LLVM_DEFINITIONS})
link_directories(${LLVM_LIBRARY_DIRS})

