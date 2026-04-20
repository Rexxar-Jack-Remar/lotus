add_library(mde INTERFACE)

option(MDE_ENABLE_PERFORMANCE_METRICS
       "Enable MDE profiling helpers"
       OFF)
option(MDE_ENABLE_DEBUG
       "Enable MDE debug checks"
       OFF)
option(MDE_DISABLE_INTEGRITY_CHECKS
       "Disable MDE integrity checks"
       OFF)
option(MDE_ENABLE_PARALLEL
       "Enable MDE std::shared_mutex-based parallel support"
       OFF)
option(MDE_ENABLE_TBB
       "Enable MDE TBB-based parallel support"
       OFF)
option(MDE_ENABLE_SERIALIZATION
       "Enable MDE nlohmann/json serialization support"
       OFF)

if(MDE_ENABLE_PARALLEL AND MDE_ENABLE_TBB)
  message(FATAL_ERROR "MDE_ENABLE_PARALLEL and MDE_ENABLE_TBB are mutually exclusive")
endif()

target_include_directories(mde INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_compile_features(mde INTERFACE cxx_std_17)

if(MDE_ENABLE_PERFORMANCE_METRICS)
  target_compile_definitions(mde INTERFACE MDE_ENABLE_PERFORMANCE_METRICS)
endif()

if(MDE_ENABLE_DEBUG)
  target_compile_definitions(mde INTERFACE MDE_ENABLE_DEBUG)
endif()

if(MDE_DISABLE_INTEGRITY_CHECKS)
  target_compile_definitions(mde INTERFACE MDE_DISABLE_INTEGRITY_CHECKS)
endif()

if(MDE_ENABLE_PARALLEL)
  target_compile_definitions(mde INTERFACE MDE_ENABLE_PARALLEL)
endif()

if(MDE_ENABLE_TBB)
  find_package(TBB REQUIRED)
  target_compile_definitions(mde INTERFACE MDE_ENABLE_TBB)
  target_link_libraries(mde INTERFACE TBB::tbb)
endif()

if(MDE_ENABLE_SERIALIZATION)
  find_package(nlohmann_json REQUIRED)
  target_compile_definitions(mde INTERFACE MDE_ENABLE_SERIALIZATION)
  target_link_libraries(mde INTERFACE nlohmann_json::nlohmann_json)
endif()
