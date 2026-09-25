add_executable(lotus-verify-mosaic lotus-verify-mosaic.cpp)
target_link_libraries(lotus-verify-mosaic PRIVATE MosaicVerification)

install(TARGETS lotus-verify-mosaic RUNTIME DESTINATION bin)
