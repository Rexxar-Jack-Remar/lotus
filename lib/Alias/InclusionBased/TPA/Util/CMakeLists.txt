# TPAUtil forwards to CanaryLLVMUtils for util::io (ReadIR/WriteIR).
# Use include/Support/LLVM/IO/ReadIR.h and WriteIR.h for the API.
add_library(TPAUtil INTERFACE)
target_link_libraries(TPAUtil INTERFACE CanaryLLVMUtils)