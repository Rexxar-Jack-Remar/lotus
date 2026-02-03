#ifndef ANALYSIS_MONO_ANALYSES_INTRAPROCEDURAL_INTRAMONO_SOLVERTEST_H_
#define ANALYSIS_MONO_ANALYSES_INTRAPROCEDURAL_INTRAMONO_SOLVERTEST_H_

#include "Dataflow/Mono/DataFlowResult.h"
#include "Dataflow/Mono/LLVMMonoAnalysisDomain.h"
#include "Dataflow/Mono/Solver/IntraMonoSolver.h"

#include <memory>
#include <set>

namespace llvm {
class Function;
class Instruction;
class Value;
} // namespace llvm

namespace mono {

struct IntraMonoSolverTestDomain : LLVMMonoAnalysisDomain<std::set<llvm::Value *>> {
};

std::unique_ptr<DataFlowResult> runIntraMonoSolverTest(llvm::Function *F);

} // namespace mono

#endif // ANALYSIS_MONO_ANALYSES_INTRAPROCEDURAL_INTRAMONO_SOLVERTEST_H_
