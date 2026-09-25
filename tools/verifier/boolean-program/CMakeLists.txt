add_executable(lotus-bool-parse BooleanProgramParseMain.c)
target_link_libraries(lotus-bool-parse PRIVATE BooleanProgramGrammar)

add_executable(lotus-bool-normalize BooleanProgramNormalizer.cpp)
