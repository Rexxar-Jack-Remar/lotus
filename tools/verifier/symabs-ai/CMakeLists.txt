# SymAbsAI analysis tool

add_executable(lotus-verify-symabs-ai lotus-verify-symabs-ai.cpp)

llvm_map_components_to_libnames(symabs_ai_tool_llvm_libs
    core irreader bitreader)

target_link_libraries(lotus-verify-symabs-ai PRIVATE
    SymAbsAIAnalysis
    ${symabs_ai_tool_llvm_libs}
    ${Z3_LIBRARIES}
)

# Ensure all registration objects from the static lib are linked (avoid dead stripping)
if(APPLE)
  # On macOS, force loading of all object files from static archives
  target_link_options(lotus-verify-symabs-ai PRIVATE -Wl,-all_load)
endif()

install(TARGETS lotus-verify-symabs-ai DESTINATION bin)
