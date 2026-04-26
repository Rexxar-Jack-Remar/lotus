add_library(CanaryDebugInfo STATIC
    DebugInfoAnalysis.cpp
    MetadataManager/MetadataEntry.cpp
    MetadataManager/LoopStructure.cpp
    MetadataManager/MetadataManager.cpp
    MetadataManager/MetadataManager_Module.cpp
    MetadataManager/MetadataManager_Instruction.cpp
    MetadataManager/MetadataManager_annotation.cpp
    UniqueIR/UniqueIRConstants.cpp
    UniqueIR/UniqueIRReader.cpp
    UniqueIR/UniqueIRMarker.cpp
    UniqueIR/UniqueIRVerifier.cpp
    UniqueIR/IDToValueMapper.cpp
    UniqueIR/UniqueIRMarkerPass.cpp
)
target_link_libraries(CanaryDebugInfo CanaryLLVMUtils)
