# SymbolicAbstraction analysis tool

add_executable(symbolic_abstraction spranalyze.cpp)

llvm_map_components_to_libnames(symbolic_abstraction_tool_llvm_libs
    core irreader bitreader)

target_link_libraries(symbolic_abstraction PRIVATE
    SymbolicAbstractionAnalysis
    ${symbolic_abstraction_tool_llvm_libs}
    ${Z3_LIBRARIES}
)

# Ensure all registration objects from the static lib are linked (avoid dead stripping)
if(APPLE)
  # On macOS, force loading of all object files from static archives
  target_link_options(symbolic_abstraction PRIVATE -Wl,-all_load)
endif()

install(TARGETS symbolic_abstraction DESTINATION bin)

