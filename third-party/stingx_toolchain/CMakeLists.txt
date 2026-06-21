cmake_minimum_required(VERSION 3.16)

project(stingx_file_runner LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED YES)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

find_path(PPL_INCLUDE_DIR ppl.hh)
find_library(PPL_LIBRARY NAMES ppl)
find_library(GMPXX_LIBRARY NAMES gmpxx)
find_library(GMP_LIBRARY NAMES gmp)
find_path(Z3_INCLUDE_DIR z3++.h)
find_library(Z3_LIBRARY NAMES z3)

if(NOT PPL_INCLUDE_DIR OR NOT PPL_LIBRARY OR NOT GMPXX_LIBRARY OR NOT GMP_LIBRARY)
    message(FATAL_ERROR
        "Missing PPL/GMP development dependencies. Install packages such as "
        "libppl-dev and libgmp-dev before building stingx_file_runner.")
endif()

if(NOT Z3_INCLUDE_DIR OR NOT Z3_LIBRARY)
    message(FATAL_ERROR
        "Missing Z3 development dependencies. Install packages such as "
        "libz3-dev before building stingx_file_runner.")
endif()

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/stingx
                 ${CMAKE_CURRENT_BINARY_DIR}/stingx_build)

target_include_directories(lstingx PUBLIC
    ${PPL_INCLUDE_DIR}
    ${Z3_INCLUDE_DIR}
)

add_executable(stingx_file_runner main.cpp)
target_include_directories(stingx_file_runner PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/stingx
    ${CMAKE_CURRENT_BINARY_DIR}/stingx_build
)
target_link_libraries(stingx_file_runner PRIVATE lstingx)
