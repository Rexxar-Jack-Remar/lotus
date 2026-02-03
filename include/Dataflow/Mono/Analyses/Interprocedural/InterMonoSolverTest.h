#ifndef ANALYSIS_MONO_ANALYSES_INTERPROCEDURAL_INTERMONO_SOLVERTEST_H_
#define ANALYSIS_MONO_ANALYSES_INTERPROCEDURAL_INTERMONO_SOLVERTEST_H_

#include "Dataflow/Mono/LLVMMonoAnalysisDomain.h"
#include "Dataflow/Mono/Solver/CallStringInterProceduralDataFlow.h"

#include <memory>
#include <set>

namespace llvm {
class Function;
class Instruction;
class Value;
} // namespace llvm

namespace mono {

struct InterMonoSolverTestDomain : LLVMMonoAnalysisDomain<std::set<llvm::Value *>> {
};

constexpr unsigned kDefaultInterMonoSolverTestCallStringLength = 2;
using InterMonoSolverTestResult =
    dataflow::ContextSensitiveDataFlowResult<
        kDefaultInterMonoSolverTestCallStringLength, std::set<llvm::Value *>>;

struct InterMonoSolverTestAnalysisResult {
  std::unique_ptr<InterMonoSolverTestResult> Results;
};

InterMonoSolverTestAnalysisResult runInterMonoSolverTest(llvm::Function *Entry);

} // namespace mono

#endif // ANALYSIS_MONO_ANALYSES_INTERPROCEDURAL_INTERMONO_SOLVERTEST_H_

