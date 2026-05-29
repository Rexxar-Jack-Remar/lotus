if(LOTUS_ENABLE_CLAM)
  add_subdirectory(clam)
endif()
if(LOTUS_ENABLE_SEAHORN)
  add_subdirectory(seahorn)
endif()
if(LOTUS_ENABLE_SMACK)
  add_subdirectory(smack)
endif()

add_subdirectory(SymAbsAI)
add_subdirectory(Sifa)
add_subdirectory(PathProgram)
add_subdirectory(Driver)
# Frontend implementations live under lib/Verification/Frontend/.
# Today this is the Boolean-program frontend consumed by NPA.
add_subdirectory(Frontend)
add_subdirectory(Transform)
add_subdirectory(Analysis)
