function(lotus_collect_build_targets directory out_var)
  get_property(targets DIRECTORY "${directory}" PROPERTY BUILDSYSTEM_TARGETS)
  get_property(children DIRECTORY "${directory}" PROPERTY SUBDIRECTORIES)
  foreach(child IN LISTS children)
    lotus_collect_build_targets("${child}" child_targets)
    list(APPEND targets ${child_targets})
  endforeach()
  set(${out_var} ${targets} PARENT_SCOPE)
endfunction()

function(lotus_apply_build_acceleration)
  if(NOT LOTUS_ENABLE_TIME_TRACE AND NOT LOTUS_PCH_TARGETS AND
     NOT LOTUS_UNITY_TARGETS)
    return()
  endif()

  # Do not change vendored or imported targets, even when they are dependencies.
  set(owned_targets)
  foreach(component lib tools tests examples)
    if(component STREQUAL "tests" AND NOT LOTUS_BUILD_TESTS)
      continue()
    endif()
    if(component STREQUAL "examples" AND NOT LOTUS_BUILD_EXAMPLES)
      continue()
    endif()
    lotus_collect_build_targets("${PROJECT_SOURCE_DIR}/${component}" targets)
    list(APPEND owned_targets ${targets})
  endforeach()
  foreach(target IN LISTS owned_targets)
    get_target_property(source_dir ${target} SOURCE_DIR)
    file(RELATIVE_PATH relative_dir "${PROJECT_SOURCE_DIR}" "${source_dir}")
    if(NOT relative_dir MATCHES "^(lib|tools|tests|examples)(/|$)")
      list(REMOVE_ITEM owned_targets ${target})
    endif()
  endforeach()

  if(LOTUS_ENABLE_TIME_TRACE)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "^(Clang|AppleClang)$")
      message(FATAL_ERROR "LOTUS_ENABLE_TIME_TRACE requires Clang or AppleClang")
    endif()
    include(CheckCXXCompilerFlag)
    check_cxx_compiler_flag(-ftime-trace LOTUS_COMPILER_HAS_TIME_TRACE)
    if(NOT LOTUS_COMPILER_HAS_TIME_TRACE)
      message(FATAL_ERROR "The selected compiler does not support -ftime-trace")
    endif()
    foreach(target IN LISTS owned_targets)
      get_target_property(type ${target} TYPE)
      if(type MATCHES "^(STATIC_LIBRARY|SHARED_LIBRARY|MODULE_LIBRARY|OBJECT_LIBRARY|EXECUTABLE)$")
        target_compile_options(${target} PRIVATE
          "$<$<COMPILE_LANGUAGE:CXX>:-ftime-trace>")
      endif()
    endforeach()
  endif()

  foreach(option_name LOTUS_PCH_TARGETS LOTUS_UNITY_TARGETS)
    foreach(target IN LISTS ${option_name})
      if(NOT target IN_LIST owned_targets)
        message(FATAL_ERROR "${option_name}: '${target}' is not a configured Lotus target")
      endif()
      get_target_property(type ${target} TYPE)
      if(NOT type MATCHES "^(STATIC_LIBRARY|SHARED_LIBRARY|MODULE_LIBRARY|OBJECT_LIBRARY|EXECUTABLE)$")
        message(FATAL_ERROR "${option_name}: '${target}' is not a compilable target")
      endif()
    endforeach()
  endforeach()

  foreach(target IN LISTS LOTUS_PCH_TARGETS)
    # Stable external headers only: changing Lotus headers should not invalidate
    # a large shared PCH. Each target owns its PCH and its exact compiler flags.
    target_precompile_headers(${target} PRIVATE
      "$<$<COMPILE_LANGUAGE:CXX>:<memory$<ANGLE-R>>"
      "$<$<COMPILE_LANGUAGE:CXX>:<string$<ANGLE-R>>"
      "$<$<COMPILE_LANGUAGE:CXX>:<vector$<ANGLE-R>>"
      "$<$<COMPILE_LANGUAGE:CXX>:<llvm/ADT/SmallVector.h$<ANGLE-R>>"
      "$<$<COMPILE_LANGUAGE:CXX>:<llvm/IR/Module.h$<ANGLE-R>>"
      "$<$<COMPILE_LANGUAGE:CXX>:<llvm/IR/Instructions.h$<ANGLE-R>>")
  endforeach()

  if(LOTUS_UNITY_TARGETS)
    if(NOT LOTUS_UNITY_BATCH_SIZE MATCHES "^[1-9][0-9]*$")
      message(FATAL_ERROR "LOTUS_UNITY_BATCH_SIZE must be a positive integer")
    endif()
    foreach(target IN LISTS LOTUS_UNITY_TARGETS)
      set_target_properties(${target} PROPERTIES
        UNITY_BUILD ON UNITY_BUILD_BATCH_SIZE ${LOTUS_UNITY_BATCH_SIZE})
    endforeach()
  endif()
endfunction()
