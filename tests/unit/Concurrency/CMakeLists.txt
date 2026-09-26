add_subdirectory(MHP)
add_subdirectory(LockSet)
add_subdirectory(ValueFlow)
add_subdirectory(Thread)
add_subdirectory(Utils/Clocks)
add_subdirectory(Utils/ThreadAPI)
add_subdirectory(Utils/ThreadLocal)
add_subdirectory(OpenMP)
add_subdirectory(MPI)
add_subdirectory(CUDA)
add_subdirectory(LinuxKernel)

# All concurrency test sources collected from the subdirectories above are
# linked into one binary so Concurrency/CanaryParallel are linked only once.
add_lotus_collected_test_suite(concurrency_tests
  LINK_LIBS Concurrency CanaryParallel)

# Preserve the established build entry point.
add_custom_target(concurrency_core_tests
  DEPENDS concurrency_tests)
