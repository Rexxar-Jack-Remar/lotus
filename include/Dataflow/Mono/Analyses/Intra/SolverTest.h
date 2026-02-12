#ifndef ANALYSIS_MONO_ANALYSES_INTRAPROCEDURAL_INTRAMONO_SOLVERTEST_H_
#define ANALYSIS_MONO_ANALYSES_INTRAPROCEDURAL_INTRAMONO_SOLVERTEST_H_

#include "Dataflow/Mono/Core/Domain.h"
#include "Dataflow/Mono/Support/Result.h"
#include "Dataflow/Mono/Solver/IntraSolver.h"

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
