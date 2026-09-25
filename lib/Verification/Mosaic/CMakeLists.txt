add_library(MosaicVerification STATIC
  Bv2IntTranslator.cpp
  CHCParser.cpp
  InstCombiner.cpp
  Int2BvOverflowAnalyzer.cpp
  Int2BvPreprocessor.cpp
  Int2BvTranslator.cpp
  MosaicFixedpoint.cpp
  MosaicUtils.cpp
  Z3Utils.cpp)

target_include_directories(MosaicVerification PUBLIC
  $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
target_link_libraries(MosaicVerification PUBLIC Z3::z3)

install(TARGETS MosaicVerification DESTINATION lib)
install(DIRECTORY ${CMAKE_SOURCE_DIR}/include/Verification/Mosaic
  DESTINATION include/Verification)
