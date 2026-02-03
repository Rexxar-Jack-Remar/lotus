#ifndef ANALYSIS_MONO_ANALYSES_INTRAPROCEDURAL_REACHABLEANALYSIS_H_
#define ANALYSIS_MONO_ANALYSES_INTRAPROCEDURAL_REACHABLEANALYSIS_H_

#include "Dataflow/Mono/DataFlowResult.h"

#include <functional>
#include <memory>

namespace llvm {
class Function;
class Instruction;
} // namespace llvm

namespace mono {

// Compute forward reachability using backward dataflow analysis.
// This analysis determines which instructions can be executed from each program point.
std::unique_ptr<DataFlowResult> runReachableAnalysis(llvm::Function *f);

std::unique_ptr<DataFlowResult> runReachableAnalysis(
    llvm::Function *f,
    const std::function<bool(llvm::Instruction *i)> &filter);

} // namespace mono

#endif // ANALYSIS_MONO_ANALYSES_INTRAPROCEDURAL_REACHABLEANALYSIS_H_
