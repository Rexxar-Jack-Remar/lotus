add_library(CanaryInterleavedDyckStagedBounds STATIC
    Grammar.cpp
    GraphOperations.cpp
    Refinement.cpp
    Regularization.cpp
    Solver.cpp
    CnfGrammar.cpp
    CnfGraph.cpp
    CnfTypes.cpp
)

target_link_libraries(CanaryInterleavedDyckStagedBounds
    PUBLIC
        CanaryInterleavedDyckCore
)
