add_library(CanarySMT STATIC
        AllSMT.cpp
        SMTConfigure.cpp
        SMTExpr.cpp
        SMTFactory.cpp
        SMTModel.cpp
        SMTObject.cpp
        SMTSolver.cpp
        CNF.cpp
        SATSolver.cpp
        SMTLIB2Solver.cpp
        Z3Expr.cpp
)

target_link_libraries(CanarySMT PUBLIC
        ${Z3_LIBRARIES}
)
