set(SMTSTABILIZER_ROOT
    ${PROJECT_SOURCE_DIR}/include/Solvers/SMT/SMTStabilizer)

find_path(SMTSTABILIZER_GMP_INCLUDE_DIR gmp.h
    HINTS ${GMP_ROOT} ENV GMP_ROOT
    PATH_SUFFIXES include
    PATHS /opt/homebrew /usr/local)
find_path(SMTSTABILIZER_MPFR_INCLUDE_DIR mpfr.h
    HINTS ${MPFR_ROOT} ENV MPFR_ROOT
    PATH_SUFFIXES include
    PATHS /opt/homebrew /usr/local)
find_library(SMTSTABILIZER_GMP_LIBRARY NAMES gmp
    HINTS ${GMP_ROOT} ENV GMP_ROOT
    PATH_SUFFIXES lib
    PATHS /opt/homebrew /usr/local)
find_library(SMTSTABILIZER_GMPXX_LIBRARY NAMES gmpxx
    HINTS ${GMP_ROOT} ENV GMP_ROOT
    PATH_SUFFIXES lib
    PATHS /opt/homebrew /usr/local)
find_library(SMTSTABILIZER_MPFR_LIBRARY NAMES mpfr
    HINTS ${MPFR_ROOT} ENV MPFR_ROOT
    PATH_SUFFIXES lib
    PATHS /opt/homebrew /usr/local)

if(NOT SMTSTABILIZER_GMP_INCLUDE_DIR OR
   NOT SMTSTABILIZER_MPFR_INCLUDE_DIR OR
   NOT SMTSTABILIZER_GMP_LIBRARY OR
   NOT SMTSTABILIZER_GMPXX_LIBRARY OR
   NOT SMTSTABILIZER_MPFR_LIBRARY)
  message(FATAL_ERROR
      "LOTUS_ENABLE_SMT_STABILIZER requires GMP, GMPXX, and MPFR. "
      "Install their development packages or set GMP_ROOT and MPFR_ROOT.")
endif()

add_library(LotusSMTStabilizer STATIC
    api/c_api.cpp
    api/stabilizer_api.cpp
    kernel/kernel.cpp
    node/node_manager.cpp
    parser/base_parser.cpp
    parser/dag.cpp
    parser/expr_parser.cpp
    parser/interval.cpp
    parser/kind.cpp
    parser/number.cpp
    parser/op_parser.cpp
    parser/opt_parser.cpp
    parser/rewrite_parser.cpp
    parser/sort.cpp
    parser/util.cpp
    parser/value.cpp
    util/bitvector.cpp
    util/gmp_utils.cpp
    util/hash.cpp
    util/node_helper.cpp)
add_library(Lotus::SMTStabilizer ALIAS LotusSMTStabilizer)

target_include_directories(LotusSMTStabilizer
    PUBLIC
      ${PROJECT_SOURCE_DIR}/include
      ${SMTSTABILIZER_ROOT}
    PRIVATE
      ${SMTSTABILIZER_ROOT}/api
      ${SMTSTABILIZER_ROOT}/kernel
      ${SMTSTABILIZER_ROOT}/node
      ${SMTSTABILIZER_ROOT}/parser
      ${SMTSTABILIZER_ROOT}/util)
target_include_directories(LotusSMTStabilizer SYSTEM PUBLIC
    ${SMTSTABILIZER_GMP_INCLUDE_DIR}
    ${SMTSTABILIZER_MPFR_INCLUDE_DIR})
target_link_libraries(LotusSMTStabilizer PUBLIC
    ${SMTSTABILIZER_MPFR_LIBRARY}
    ${SMTSTABILIZER_GMPXX_LIBRARY}
    ${SMTSTABILIZER_GMP_LIBRARY})
set_target_properties(LotusSMTStabilizer PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF)
