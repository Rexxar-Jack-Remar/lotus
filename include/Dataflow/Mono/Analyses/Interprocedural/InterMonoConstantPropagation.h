#ifndef ANALYSIS_MONO_ANALYSES_INTERPROCEDURAL_INTERMONO_CONSTANTPROPAGATION_H_
#define ANALYSIS_MONO_ANALYSES_INTERPROCEDURAL_INTERMONO_CONSTANTPROPAGATION_H_

#include "Dataflow/Mono/Analyses/Intraprocedural/IntraMonoConstantPropagation.h"
#include "Dataflow/Mono/Solver/CallStringInterProceduralDataFlow.h"

#include <memory>

namespace llvm {
class Function;
} // namespace llvm

namespace mono {

constexpr unsigned kDefaultConstantPropagationCallStringLength = 2;
using InterMonoConstantPropagationResult =
    dataflow::ContextSensitiveDataFlowResult<
        kDefaultConstantPropagationCallStringLength, ConstantPropagationMap>;

struct InterMonoConstantPropagationAnalysisResult {
  std::unique_ptr<InterMonoConstantPropagationResult> Results;
};

// Interprocedural constant propagation (call-string length is fixed at 2).
InterMonoConstantPropagationAnalysisResult
runInterMonoConstantPropagation(llvm::Function *Entry);

} // namespace mono

#endif // ANALYSIS_MONO_ANALYSES_INTERPROCEDURAL_INTERMONO_CONSTANTPROPAGATION_H_

