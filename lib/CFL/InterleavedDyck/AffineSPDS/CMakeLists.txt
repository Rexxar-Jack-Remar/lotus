if(NOT TARGET CanaryInterleavedDyckSPDS)
    message(FATAL_ERROR "AffineSPDS requires the SPDS engine. Add SPDS before AffineSPDS.")
endif()
add_library(CanaryInterleavedDyckAffineSPDS STATIC
    Matrix.cpp
    Affine.cpp
    Observer.cpp
    Solver.cpp)
target_link_libraries(CanaryInterleavedDyckAffineSPDS PUBLIC
    CanaryInterleavedDyckSPDS
    LLVMSupport)
