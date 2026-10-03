foreach(family IN LISTS LOTUS_SELECTED_TOOL_FAMILIES)
  add_subdirectory(${family})
endforeach()
