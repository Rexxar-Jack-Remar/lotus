add_subdirectory(PDG)
add_subdirectory(GVFG)
add_subdirectory(ShadowMemSSA)
add_subdirectory(GSA)
add_subdirectory(SVFG)

# All IR test sources collected from the subdirectories above are linked into
# one binary so the shared libraries are linked only once.
lotus_collect_test_sources(ir_tests GraphViewTest.cpp)

add_lotus_collected_test_suite(ir_tests
  LINK_LIBS
    CanaryICFG
    CanaryPDG
    CanaryAliasAnalysisWrapper
    CanaryGSA
    CanaryGuardedValueFlow
    CanaryShadowMemSSA
    CanaryOptimizationIPO
    SVFG
    ${Z3_LIBRARIES})

# No IR-specific helper exists in this checkout; keep this target's link set small.
add_lotus_targeted_test(usetracessa_objects_test UseTraceSSA/FactorizedTest.cpp
  LINK_LIBS CanaryUseTraceSSACore)
add_lotus_targeted_test(usetracessa_scaling_test UseTraceSSA/ScalingTest.cpp
  LINK_LIBS CanaryUseTraceSSACore)
add_lotus_targeted_test(usetracessa_native_test UseTraceSSA/NativeFactorizedTest.cpp
  LINK_LIBS CanaryUseTraceSSASVFG)
add_lotus_targeted_test(ufg_test UFG/UFGTest.cpp
  LINK_LIBS CanaryUFG CanaryUseTraceSSASVFG LLVMAsmParser)
