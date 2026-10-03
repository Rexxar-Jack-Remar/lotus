# Saber Checker Library
# Migrated from SVF's SABER engine to Lotus

set(Saber_SOURCES
  SaberSVFGBuilder.cpp
  SrcSnkDDA.cpp
  SaberCondAllocator.cpp
  SaberOptions.cpp
  ProgSlice.cpp
  LeakChecker.cpp
  DoubleFreeChecker.cpp
  UseAfterFreeChecker.cpp
  FileChecker.cpp
  SaberCheckerAPI.cpp
  Z3Expr.cpp
)

add_library(Saber STATIC ${Saber_SOURCES})
# Public Saber headers contain Z3Expr fields whose layout depends on USE_Z3.
# Consumers constructing Saber checkers must use the same definition.
target_compile_definitions(Saber PUBLIC USE_Z3)
target_link_libraries(Saber
  PUBLIC
    SVFG
    CanaryICFG
    DDA
    CheckerFramework
    ${Z3_LIBRARIES}
)
