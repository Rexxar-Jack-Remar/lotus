llvm_map_components_to_libnames(checker_tooling_llvm_libs Core Support)

# Shared tool-support library for the checker command-line frontend
add_library(CanaryCheckerTooling STATIC CheckerOptions.cpp CheckerReport.cpp)
target_link_libraries(CanaryCheckerTooling PUBLIC CheckerFramework CanaryFuzzing spdlog::spdlog ${checker_tooling_llvm_libs})