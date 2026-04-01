add_library(CanaryNisse STATIC
        Edge.cpp
        NisseAnalysis.cpp
        NissePass.cpp
        NissePlugin.cpp
        prof.c
)

target_link_libraries(CanaryNisse
        CanaryADT
        LLVMAnalysis
        LLVMCore
        LLVMSupport
)
