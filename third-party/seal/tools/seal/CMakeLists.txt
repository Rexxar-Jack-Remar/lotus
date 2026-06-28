set(LLVM_LINK_COMPONENTS
    AggressiveInstCombine
    Analysis
    AsmParser
    AsmPrinter
    BinaryFormat
    BitReader
    BitWriter
    BitstreamReader
    CodeGen
    Core
    Coroutines
    Demangle
    FrontendOpenMP
    IRReader
    InstCombine
    Instrumentation
    LTO
    Linker
    MC
    MCParser
    MIRParser
    Object
    ObjectYAML
    Option
    Passes
    ProfileData
    Remarks
    ScalarOpts
    Support
    Target
    TransformUtils
    Vectorize
    ipo
)

# Convert component names to library paths (works with find_package(LLVM CONFIG))
llvm_map_components_to_libnames(_llvm_libs ${LLVM_LINK_COMPONENTS})

add_executable(seal seal.cpp LiftingFiniteStateMachinePass.cpp)
target_link_libraries(seal PRIVATE
    PPYCore PPYMemory PPYSupport PPYTransform
    ${_llvm_libs}
    ${Z3_LIBRARIES}
)

if(APPLE)
    target_link_libraries(seal PRIVATE pthread ncurses)
elseif(UNIX)
    target_link_libraries(seal PRIVATE pthread ncurses dl)
endif()
