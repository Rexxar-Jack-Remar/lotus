set(TransformSourceCodes 
ExpandByVal.cpp ExpandConstantExpr.cpp 
ExpandGetElementPtr.cpp 
ExpandIndirectBr.cpp 
ExpandUtils.cpp 
FoldIntToPtr.cpp 
GlobalCleanup.cpp 
LowerGlobalCtor.cpp 
RunPrepass.cpp)

add_library(TPATransforms STATIC ${TransformSourceCodes})

#target_link_libraries(TPATransforms LLVMTransformUtils LLVMipo LLVMSupport LLVMCore)
