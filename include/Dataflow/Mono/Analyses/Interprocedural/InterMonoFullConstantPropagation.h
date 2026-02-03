#ifndef ANALYSIS_MONO_ANALYSES_INTERPROCEDURAL_INTERMONO_FULLCONSTANTPROPAGATION_H_
#define ANALYSIS_MONO_ANALYSES_INTERPROCEDURAL_INTERMONO_FULLCONSTANTPROPAGATION_H_

#include "Dataflow/Mono/Analyses/Intraprocedural/IntraMonoFullConstantPropagation.h"
#include "Dataflow/Mono/Solver/CallStringInterProceduralDataFlow.h"

#include <memory>

namespace llvm {
class Function;
} // namespace llvm

namespace mono {

constexpr unsigned kDefaultFullConstantPropagationCallStringLength = 2;
using InterMonoFullConstantPropagationResult =
    dataflow::ContextSensitiveDataFlowResult<
        kDefaultFullConstantPropagationCallStringLength,
        FullConstantPropagationState>;

struct InterMonoFullConstantPropagationAnalysisResult {
  std::unique_ptr<InterMonoFullConstantPropagationResult> Results;
};

InterMonoFullConstantPropagationAnalysisResult
runInterMonoFullConstantPropagation(llvm::Function *Entry);

} // namespace mono

#endif // ANALYSIS_MONO_ANALYSES_INTERPROCEDURAL_INTERMONO_FULLCONSTANTPROPAGATION_H_

