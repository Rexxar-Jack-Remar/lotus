# K-Induction engine (reuses PathBMC when HAVE_CLAM; stub otherwise).
add_library(seahorn.KInduction STATIC
  KInductionPass.cc
  )
if(HAVE_CLAM)
  target_compile_definitions(seahorn.KInduction PRIVATE HAVE_CLAM=1)
endif()

target_compile_options(seahorn.KInduction PRIVATE -Wno-deprecated-declarations)

set_target_properties(seahorn.KInduction PROPERTIES OUTPUT_NAME "seahorn-kinduction")
