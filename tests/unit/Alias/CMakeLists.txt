# Alias Analysis Unit Tests

add_subdirectory(DemandDriven/DDA)
add_subdirectory(InclusionBased/AserPTA)
add_subdirectory(InclusionBased/BootstrapAA)
add_subdirectory(InclusionBased/FlowSensitive)
add_subdirectory(InclusionBased/LotusAA)
add_subdirectory(InclusionBased/SparrowAA)
add_subdirectory(InclusionBased/TPA)
add_subdirectory(UnificationBased/DyckAA)
add_subdirectory(UnificationBased/SeaDsa)
add_subdirectory(Specialized/AllocAA)
if(LOTUS_ENABLE_TYPE_QUALIFIER)
  add_subdirectory(Specialized/TypeQualifier)
endif()
add_subdirectory(Specialized/UnderApproxAA)
add_subdirectory(Infrastructure/PtsSet)

# All alias-analysis test sources declared by the subdirectories above are
# collected into one binary so the LLVM/Lotus libraries are linked only once.
set(LOTUS_ALIAS_TEST_LIBS
    DDA
    AserPTA
    CanaryBootstrapAA
    FlowSensitivePTA
    Andersen
    TPATransforms
    FSCSPointerAnalysis
    CanaryAliasAnalysisWrapper
    LotusAA
    CanaryGuardedValueFlow
    CanaryAliasPtsSet
    CanaryAllocAA
    CanaryUnderApproxAA
    CanaryDyckAA
    SeaDsaAnalysis
    ${Z3_LIBRARIES})
if(LOTUS_ENABLE_TYPE_QUALIFIER)
  list(APPEND LOTUS_ALIAS_TEST_LIBS TypeQualifier)
endif()

add_lotus_collected_test_suite(alias_tests
  LINK_LIBS ${LOTUS_ALIAS_TEST_LIBS})

# The BootstrapAA CLI smoke test needs its tool to be built first.
add_dependencies(alias_tests lotus-alias-bootstrap)
