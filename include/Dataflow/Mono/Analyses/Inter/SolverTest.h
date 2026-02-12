#ifndef ANALYSIS_MONO_ANALYSES_INTERPROCEDURAL_INTERMONO_SOLVERTEST_H_
#define ANALYSIS_MONO_ANALYSES_INTERPROCEDURAL_INTERMONO_SOLVERTEST_H_

#include "Dataflow/Mono/Container/Traits.h"
#include "Dataflow/Mono/Core/Domain.h"
#include "Dataflow/Mono/Core/CallStringSolver.h"

#include <memory>

namespace llvm {
class Function;
class Instruction;
class Value;
} // namespace llvm

namespace mono {

struct InterMonoSolverTestDomain : LLVMMonoAnalysisDomain<SetContainer<llvm::Value *>> {
};

constexpr unsigned kDefaultInterMonoSolverTestCallStringLength = 2;
using InterMonoSolverTestResult =
    dataflow::ContextSensitiveDataFlowResult<
        kDefaultInterMonoSolverTestCallStringLength, SetContainer<llvm::Value *>>;

struct InterMonoSolverTestAnalysisResult {
  std::unique_ptr<InterMonoSolverTestResult> Results;
};

InterMonoSolverTestAnalysisResult runInterMonoSolverTest(llvm::Function *Entry);

} // namespace mono

#endif // ANALYSIS_MONO_ANALYSES_INTERPROCEDURAL_INTERMONO_SOLVERTEST_H_

