add_library(CanaryAllocAA STATIC
        AllocAA.cpp
        MemorySite.cpp
)

target_link_libraries(CanaryAllocAA PUBLIC AliasSpecManager)