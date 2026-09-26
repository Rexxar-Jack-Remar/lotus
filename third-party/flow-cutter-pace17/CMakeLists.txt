file(GLOB FLOW_CUTTER_2017_SOURCES CONFIGURE_DEPENDS src/*.cpp)
add_executable(lotus-flow-cutter-treewidth ${FLOW_CUTTER_2017_SOURCES})
set_target_properties(lotus-flow-cutter-treewidth PROPERTIES
  OUTPUT_NAME flow_cutter_pace17)
