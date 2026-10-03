# Every CFL gtest source is collected into one binary so the shared CFL/Lotus
# libraries are linked only once. The child directories mirror the lib/CFL
# subsystems (Classical, CSIndex, DynamicDyck, InterleavedDyck) and contribute
# their sources together with their CLI/regression cases.
add_subdirectory(Classical)
add_subdirectory(CSIndex)
add_subdirectory(DynamicDyck)
add_subdirectory(InterleavedDyck)

add_lotus_collected_test_suite(cfl_tests
  TIMEOUT 240
  LINK_LIBS
    CanaryDynamicDyck
    CanaryInterleavedDyckLCL
    CanaryInterleavedDyckSPDS
    CanaryInterleavedDyckAffineSPDS
    CanaryInterleavedDyckUnary
    CanaryInterleavedDyckStagedBounds
    CanaryInterleavedDyckMCFL
    CanaryClassicalCFL
    CanaryClassicalCFLAliasClient
    CanaryClassicalCFLValueFlowClient
    CanaryCSIndexSCS
  COMPILE_DEFINITIONS
    INTERLEAVED_DYCK_DATASET_DIR="${CMAKE_SOURCE_DIR}/benchmarks/real-world/CFL/InterleavedDyck/taint"
    MCFL_DATASET_DIR="${CMAKE_SOURCE_DIR}/benchmarks/real-world/CFL/InterleavedDyck/taint")
