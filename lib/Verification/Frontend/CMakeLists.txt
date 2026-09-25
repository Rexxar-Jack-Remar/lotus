add_library(VerificationFrontend STATIC
  BooleanProgram.cpp
  BooleanProgramParser.cpp
  PredicateProgramLowering.cpp
)

target_link_libraries(VerificationFrontend
  PUBLIC
  NPADataFlow
  LLVMSupport
)
if(LOTUS_ENABLE_BOOLEAN_PROGRAM_TOOLS)
  find_package(BISON REQUIRED)
  find_package(FLEX REQUIRED)
  BISON_TARGET(BooleanProgramGrammar bp_parser.yy
    ${CMAKE_CURRENT_BINARY_DIR}/bool.tab.c
    DEFINES_FILE ${CMAKE_CURRENT_BINARY_DIR}/bool.tab.h)
  FLEX_TARGET(BooleanProgramScanner bp_scanner.ll
    ${CMAKE_CURRENT_BINARY_DIR}/lex.yy.c)
  ADD_FLEX_BISON_DEPENDENCY(BooleanProgramScanner BooleanProgramGrammar)
  add_library(BooleanProgramGrammar STATIC
    ${BISON_BooleanProgramGrammar_OUTPUTS}
    ${FLEX_BooleanProgramScanner_OUTPUTS})
  target_include_directories(BooleanProgramGrammar PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
endif()
