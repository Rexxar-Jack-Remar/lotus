#include "Analysis/SymbolicExecution/AnalysisLimit.h"

using namespace SymbolicExecution;

unsigned AnalysisLimit::INST_QUERY_LIMIT_V = 5;
unsigned AnalysisLimit::FUNC_QUERY_LIMIT_V = 200;
unsigned AnalysisLimit::VALUE_SET_LIMIT_V = 20;
unsigned AnalysisLimit::SYMBOLIC_VAL_SET_LIMIT_V = 20;
unsigned AnalysisLimit::POINTS_SET_LIMIT_V = 20;
unsigned AnalysisLimit::TAINT_VAL_SET_LIMIT_V = 30000;
unsigned AnalysisLimit::FUNC_INLINE_LIMIT_V = 10;
unsigned AnalysisLimit::CONSTRAINT_SIZE_LIMIT_V = 1000;
unsigned AnalysisLimit::MAX_FUNC_SOLVER_LIMIT_V = 1024;
