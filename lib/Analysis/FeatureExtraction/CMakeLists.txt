add_library(CanaryML STATIC
  MemoryMLFeatures.cpp
)

# Link against SeaDsaAnalysis and LLVM libraries
target_link_libraries(CanaryML PUBLIC
  SeaDsaAnalysis
)

# Install library
install(TARGETS CanaryML
  LIBRARY DESTINATION lib
  ARCHIVE DESTINATION lib
)
