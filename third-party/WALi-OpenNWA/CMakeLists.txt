cmake_minimum_required(VERSION 3.10)
if(POLICY CMP0167)
  cmake_policy(SET CMP0167 NEW)
endif()

project(WALiOpenNWA
  VERSION 1.0
  LANGUAGES C CXX)

option(WALI_BUILD_SHARED "Build WALi libraries as shared libraries" ON)
option(WALI_BUILD_TOOLS "Build OpenNWA command-line tools" ON)
option(WALI_BUILD_ADDONS "Build optional add-on libraries" OFF)
option(WALI_BUILD_EXAMPLES "Build example programs" OFF)

set(WALI_CHECKING "fast" CACHE STRING "Checking level: slow, fast, or none")
set_property(CACHE WALI_CHECKING PROPERTY STRINGS slow fast none)

if(WALI_CHECKING STREQUAL "slow")
  set(WALI_CHECKED_LEVEL 2)
elseif(WALI_CHECKING STREQUAL "none")
  set(WALI_CHECKED_LEVEL 0)
else()
  set(WALI_CHECKED_LEVEL 1)
endif()

find_package(Boost REQUIRED)

if(WALI_BUILD_SHARED)
  set(WALI_LIBRARY_TYPE SHARED)
else()
  set(WALI_LIBRARY_TYPE STATIC)
endif()

add_library(wali ${WALI_LIBRARY_TYPE})
add_library(WALi::wali ALIAS wali)

set_target_properties(wali PROPERTIES
  CXX_STANDARD 11
  CXX_STANDARD_REQUIRED YES
  CXX_EXTENSIONS YES
  OUTPUT_NAME wali)

target_compile_definitions(wali
  PUBLIC
    BOOST_NO_DEFAULTED_FUNCTIONS=1
    CHECKED_LEVEL=${WALI_CHECKED_LEVEL})

target_include_directories(wali
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Source>"
    "$<INSTALL_INTERFACE:include>")

target_link_libraries(wali PUBLIC Boost::boost)

if(UNIX AND NOT APPLE)
  target_link_libraries(wali PUBLIC rt)
endif()

file(GLOB_RECURSE WALI_SOURCE_FILES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/Source/wali/*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/Source/opennwa/*.cpp")

set(WALI_EXCLUDED_SOURCES
  "${CMAKE_CURRENT_SOURCE_DIR}/Source/opennwa/NWA-tests.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/Source/opennwa/OpenFstInterop.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/Source/opennwa/ProcedureNWAs.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/Source/opennwa/RelationOps.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/Source/opennwa/query/details/iterator_tests.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/Source/opennwa/query/details/test_selectors.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/Source/wali/details/PriorityWorklist.boost_heap.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/Source/wali/details/PriorityWorklist.multiset.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/Source/wali/graph/Experimental.cpp")
list(REMOVE_ITEM WALI_SOURCE_FILES ${WALI_EXCLUDED_SOURCES})

target_sources(wali PRIVATE ${WALI_SOURCE_FILES})

function(wali_add_opennwa_tool name)
  add_executable(${name} ${ARGN})
  target_link_libraries(${name} PRIVATE WALi::wali)
  set_target_properties(${name} PROPERTIES
    CXX_STANDARD 11
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS YES)
endfunction()

function(wali_add_single_nwa_tool command)
  set(options)
  set(one_value_args FUNCTION NAMESPACE)
  cmake_parse_arguments(ARG "${options}" "${one_value_args}" "" ${ARGN})
  if(NOT ARG_FUNCTION)
    set(ARG_FUNCTION "${command}")
  endif()
  if(NOT ARG_NAMESPACE)
    set(ARG_NAMESPACE "construct")
  endif()

  wali_add_opennwa_tool(nwa-${command}
    "${CMAKE_CURRENT_SOURCE_DIR}/Source/bin/opennwa/single-nwa-command.cpp")
  target_compile_definitions(nwa-${command} PRIVATE
    CONSTRUCTION_FUNCTION=${ARG_FUNCTION}
    CONSTRUCTION_NAMESPACE=${ARG_NAMESPACE})
endfunction()

function(wali_add_double_nwa_tool command)
  set(options)
  set(one_value_args FUNCTION NAMESPACE)
  cmake_parse_arguments(ARG "${options}" "${one_value_args}" "" ${ARGN})
  if(NOT ARG_FUNCTION)
    set(ARG_FUNCTION "${command}")
  endif()
  if(NOT ARG_NAMESPACE)
    set(ARG_NAMESPACE "construct")
  endif()

  wali_add_opennwa_tool(nwa-${command}
    "${CMAKE_CURRENT_SOURCE_DIR}/Source/bin/opennwa/double-nwa-command.cpp")
  target_compile_definitions(nwa-${command} PRIVATE
    CONSTRUCTION_FUNCTION=${ARG_FUNCTION}
    CONSTRUCTION_NAMESPACE=${ARG_NAMESPACE})
endfunction()

if(WALI_BUILD_TOOLS)
  set(OPENNWA_TOOL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/Source/bin/opennwa")

  wali_add_opennwa_tool(nwa-print-stats "${OPENNWA_TOOL_DIR}/print-stats.cpp")
  wali_add_opennwa_tool(nwa-print-dot "${OPENNWA_TOOL_DIR}/print-dot.cpp")
  wali_add_opennwa_tool(nwa-is-empty "${OPENNWA_TOOL_DIR}/is-empty.cpp")
  wali_add_opennwa_tool(nwa-get-example "${OPENNWA_TOOL_DIR}/get-example.cpp")
  wali_add_opennwa_tool(nwa-is-member "${OPENNWA_TOOL_DIR}/is-member.cpp")
  wali_add_opennwa_tool(nwa-language-equals "${OPENNWA_TOOL_DIR}/language-equals.cpp")
  wali_add_opennwa_tool(nwa-language-subseteq "${OPENNWA_TOOL_DIR}/language-subseteq.cpp")

  wali_add_double_nwa_tool(intersect)
  wali_add_double_nwa_tool(union FUNCTION unionNwa)
  wali_add_double_nwa_tool(concat)

  wali_add_single_nwa_tool(star)
  wali_add_single_nwa_tool(complement)
  wali_add_single_nwa_tool(determinize)
  wali_add_single_nwa_tool(reverse)
  wali_add_single_nwa_tool(expand FUNCTION expandNwa NAMESPACE experimental)

  include(CheckIncludeFileCXX)
  set(CMAKE_REQUIRED_INCLUDES ${Boost_INCLUDE_DIRS})
  check_include_file_cxx("boost/filesystem/convenience.hpp" WALI_HAS_BOOST_FILESYSTEM_CONVENIENCE)
  unset(CMAKE_REQUIRED_INCLUDES)

  find_package(Boost QUIET COMPONENTS filesystem system)
  if(WALI_HAS_BOOST_FILESYSTEM_CONVENIENCE AND Boost_FILESYSTEM_FOUND AND Boost_SYSTEM_FOUND)
    wali_add_opennwa_tool(nwa-synchronize-alphabets
      "${OPENNWA_TOOL_DIR}/synchronize-alphabets.cpp")
    target_link_libraries(nwa-synchronize-alphabets PRIVATE
      Boost::filesystem Boost::system)
  endif()
endif()

if(WALI_BUILD_ADDONS)
  find_package(Boost REQUIRED COMPONENTS serialization)

  file(GLOB BUDDY_C_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/AddOns/Domains/ThirdParty/buddy-2.4/src/*.c")
  set(BUDDY_GENERATED_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated-buddy-include")
  file(MAKE_DIRECTORY "${BUDDY_GENERATED_INCLUDE_DIR}/buddy")
  file(GLOB BUDDY_PUBLIC_HEADERS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/AddOns/Domains/ThirdParty/buddy-2.4/include/*.h")
  foreach(BUDDY_HEADER ${BUDDY_PUBLIC_HEADERS})
    get_filename_component(BUDDY_HEADER_NAME "${BUDDY_HEADER}" NAME)
    configure_file("${BUDDY_HEADER}"
      "${BUDDY_GENERATED_INCLUDE_DIR}/buddy/${BUDDY_HEADER_NAME}"
      COPYONLY)
  endforeach()

  add_library(bdd STATIC
    ${BUDDY_C_SOURCES}
    "${CMAKE_CURRENT_SOURCE_DIR}/AddOns/Domains/ThirdParty/buddy-2.4/src/cppext.cxx")
  target_include_directories(bdd PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/AddOns/Domains/ThirdParty/buddy-2.4/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/AddOns/Domains/ThirdParty/buddy-2.4/include"
    "${BUDDY_GENERATED_INCLUDE_DIR}")
  target_compile_options(bdd PRIVATE
    $<$<COMPILE_LANGUAGE:C>:-O2>)

  find_package(glog QUIET)
  if(NOT TARGET glog::glog)
    find_path(GLOG_INCLUDE_DIR glog/logging.h)
    find_library(GLOG_LIBRARY NAMES glog)
    if(GLOG_INCLUDE_DIR AND GLOG_LIBRARY)
      add_library(glog::glog UNKNOWN IMPORTED)
      set_target_properties(glog::glog PROPERTIES
        IMPORTED_LOCATION "${GLOG_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${GLOG_INCLUDE_DIR}")
    endif()
  endif()

  if(NOT TARGET glog::glog)
    message(WARNING
      "glog was not found. Building bundled bdd only; walidomains and xfa are skipped.")
  else()
  add_library(walidomains ${WALI_LIBRARY_TYPE}
    "${CMAKE_CURRENT_SOURCE_DIR}/AddOns/Domains/Source/wali/domains/binrel/BinRel.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/AddOns/Domains/Source/wali/domains/binrel/ProgramBddContext.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/AddOns/Domains/Source/wali/domains/binrel/nwa_detensor.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/AddOns/Domains/Source/wali/domains/reach/Reach.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/AddOns/Domains/Source/wali/domains/lh/LH.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/AddOns/Domains/Source/wali/domains/lh/PhaseLH.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/AddOns/Domains/Source/wali/domains/matrix/Matrix.cpp")
  set_target_properties(walidomains PROPERTIES
    CXX_STANDARD 11
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS YES)
  target_include_directories(walidomains PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/AddOns/Domains/Source"
    "${BUDDY_GENERATED_INCLUDE_DIR}")
  target_link_libraries(walidomains PUBLIC WALi::wali bdd Boost::serialization glog::glog)

  add_library(xfa ${WALI_LIBRARY_TYPE}
    "${CMAKE_CURRENT_SOURCE_DIR}/AddOns/Xfa/Source/wali/xfa/Xfa.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/AddOns/Xfa/Source/wali/xfa/convert_xfa.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/AddOns/Xfa/Source/wali/xfa/xfa_livevar_analysis.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/AddOns/Xfa/Source/wali/util/read_file.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/AddOns/Xfa/Source/wali/util/base64.cpp")
  set_target_properties(xfa PROPERTIES
    CXX_STANDARD 11
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS YES)
  target_include_directories(xfa PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/AddOns/Xfa/Source")
  target_link_libraries(xfa PUBLIC walidomains)
  endif()
endif()

if(WALI_BUILD_EXAMPLES)
  wali_add_opennwa_tool(ex-reach
    "${CMAKE_CURRENT_SOURCE_DIR}/Examples/Reach/Reach.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Examples/Reach/ExReach.cpp")
  wali_add_opennwa_tool(ex-reach-wit
    "${CMAKE_CURRENT_SOURCE_DIR}/Examples/Reach/Reach.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Examples/Reach/ExReachWit.cpp")
endif()

include(GNUInstallDirs)

install(TARGETS wali
  EXPORT WALiTargets
  ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")

install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/Source/"
  DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
  FILES_MATCHING PATTERN "*.hpp" PATTERN "*.h")

install(EXPORT WALiTargets
  NAMESPACE WALi::
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/WALi")
