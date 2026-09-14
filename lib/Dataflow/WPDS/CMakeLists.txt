# WPDS-based interprocedural dataflow analysis implementation

# Framework core
set(WPDS_FRAMEWORK_SOURCES
  Backend.cpp
  PreparedAnalysis.cpp
  Backend/Model.cpp
  Backend/PreparedBackend.cpp
  Backend/LegacyBackend.cpp
  Core/DataFlowFacts.cpp
  Core/GenKillValue.cpp
  Core/GenKillTransformer.cpp
  Solver/InterProceduralDataFlowEngine.cpp
)

# Client analyses
set(WPDS_CLIENT_SOURCES
  Analyses/UninitializedVariablesAnalysis.cpp
  Analyses/ConstantPropagationAnalysis.cpp
  Analyses/LivenessAnalysis.cpp
  Analyses/TaintAnalysis.cpp
)

set(WPDS_SOURCES ${WPDS_FRAMEWORK_SOURCES} ${WPDS_CLIENT_SOURCES})

add_library(WPDSDataFlow STATIC ${WPDS_SOURCES})

if(LOTUS_ENABLE_WALI_OPENNWA)
  target_sources(WPDSDataFlow PRIVATE Backend/WaliBackend.cpp)
  target_compile_definitions(WPDSDataFlow PUBLIC LOTUS_ENABLE_WALI_OPENNWA=1)
  target_link_libraries(WPDSDataFlow PRIVATE wali)
endif()

# Optional witness tracing support in the WPDS solver (requires WITNESS_TRACE).
if(LOTUS_WPDS_WITNESS_TRACE)
  target_compile_definitions(WPDSDataFlow PRIVATE WITNESS_TRACE)
endif()
