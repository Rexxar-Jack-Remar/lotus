set (USED_LIBS
  seahorn.LIB
  SeaInstrumentation
  SeaTransformsScalar
  SeaTransformsUtils
  SeaAnalysis
  SeaSmt
  ${SEA_DSA_LIBS}
  SeaSupport
  ${LLVM_SEAHORN_LIBS}
  # SeaLlvmIpo  # Not available - from llvm-seahorn which wasn't migrated
  ${CLAM_LIBS}
  ${Z3_LIBRARIES}
  ${GMP_LIB}
  ${RT_LIB}
  seahorn.KInduction
  )


set(LLVM_LINK_COMPONENTS
  #BitWriter
  #IRReader
  IPO
  #ScalarOpts
  Instrumentation
  ObjCARCOpts
  #Target
  )
add_executable(seahorn seahorn.cpp)
if(APPLE)
  set_target_properties(seahorn PROPERTIES LINK_FLAGS "${LINK_FLAGS} -Wl,-search_paths_first")
endif()
# Add include directories needed by seahorn executable
target_include_directories(seahorn PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/include/Apps
  ${CMAKE_BINARY_DIR}/include
  ${CMAKE_BINARY_DIR}/include/Apps
)
# Link LLVM components first to establish base dependencies
llvm_config (seahorn ${LLVM_LINK_COMPONENTS})
# Then link our custom libraries
target_link_libraries (seahorn PRIVATE ${USED_LIBS})
# Suppress deprecation warnings
target_compile_options(seahorn PRIVATE -Wno-deprecated-declarations)
install(TARGETS seahorn RUNTIME DESTINATION bin)

if (SEAHORN_STATIC_EXE)
  set (CMAKE_EXE_LINKER_FLAGS "-static -static-libgcc -static-libstdc++")
  set_target_properties (seahorn PROPERTIES LINK_SEARCH_START_STATIC ON)
  set_target_properties (seahorn PROPERTIES LINK_SEARCH_END_STATIC ON)
endif()

