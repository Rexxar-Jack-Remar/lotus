# SymAbsAI: Static Program Analysis Toolkit Using Symbolic Abstraction + Abstract Interpretation
# Integrated into Lotus framework
#
# This is a PROGRAM-LEVEL analysis framework for LLVM IR with abstract interpretation.
# It provides fixpoint engines, abstract domains, and LLVM pass integration.
#
# ⚠️ This is NOT the same as SymAbs (lib/Solvers/SMT/SymAbs), which provides
# FORMULA-LEVEL abstraction algorithms for SMT formulas.
#
# See README.md for details on the distinction.

# SymAbsAI library sources
set(SYMABS_AI_SOURCES
    # Analyzers
    Core/Engine/Analyzer.cpp
    Core/Engine/UnilateralAnalyzer.cpp
    Core/Engine/BilateralAnalyzer.cpp
    Core/Engine/OMTAnalyzer.cpp
    # Core components
    Core/Foundation/Fragment.cpp
    Core/Foundation/FragmentDecomposition.cpp
    Core/Foundation/Expression.cpp
    Core/Foundation/ConcreteState.cpp
    Core/Foundation/InstructionSemantics.cpp
    Core/Foundation/MemoryModel.cpp
    Core/Foundation/FloatingPointModel.cpp
    Core/Integration/FunctionContext.cpp
    Core/Integration/ModuleContext.cpp
    Core/Foundation/RepresentedValue.cpp
    Core/Foundation/ValueMapping.cpp
    Core/Foundation/DomainConstructor.cpp
    Core/Integration/SymAbsAIPass.cpp
    Core/Integration/Checks.cpp
    Core/Integration/ResultStore.cpp
    Core/Foundation/ParamStrategy.cpp
    # Utilities
    Utils/Utils.cpp
    Utils/Config.cpp
    Utils/PrettyPrinter.cpp
    Utils/Reporting.cpp
    Utils/Z3APIExtension.cpp
    # Abstract Domains
    Domains/NumRels.cpp
    Domains/SimpleConstProp.cpp
    Domains/BitMask.cpp
    Domains/Predicates.cpp
    Domains/Intervals.cpp
    Domains/MemRange.cpp
    Domains/MemRegions.cpp
    Domains/Boolean.cpp
    Domains/Congruence.cpp
    Domains/Zones.cpp
)

# Create SymAbsAI library
add_library(SymAbsAIAnalysis STATIC ${SYMABS_AI_SOURCES})

# Link LLVM libraries
llvm_map_components_to_libnames(symabs_ai_llvm_libs
    asmparser bitreader bitwriter scalaropts transformutils)

target_link_libraries(SymAbsAIAnalysis PUBLIC
    ${Z3_LIBRARIES}
    ${symabs_ai_llvm_libs}
)

# Installation
install(TARGETS SymAbsAIAnalysis DESTINATION lib)
