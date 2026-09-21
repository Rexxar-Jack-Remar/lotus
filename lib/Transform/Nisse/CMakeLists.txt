add_library(CanaryNisse STATIC
        Edge.cpp
        NisseAnalysis.cpp
        NissePass.cpp
        PRUE.cpp
        NissePlugin.cpp
        Prof.c
)

target_link_libraries(CanaryNisse
        CanaryADT
        LLVMAnalysis
        LLVMCore
        LLVMTransformUtils
        LLVMSupport
)
