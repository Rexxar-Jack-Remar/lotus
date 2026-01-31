#ifndef ANALYSIS_MONO_ANALYSES_INTRAPROCEDURAL_INTRAMONO_UNINITVARIABLES_H_
#define ANALYSIS_MONO_ANALYSES_INTRAPROCEDURAL_INTRAMONO_UNINITVARIABLES_H_

#include "Dataflow/Mono/DataFlowResult.h"

#include <memory>

namespace llvm {
class Function;
} // namespace llvm

namespace mono {

// Forward uninitialized variables analysis (intraprocedural).
[[nodiscard]] std::unique_ptr<DataFlowResult>
runIntraMonoUninitVariables(llvm::Function *F);

} // namespace mono

#endif // ANALYSIS_MONO_ANALYSES_INTRAPROCEDURAL_INTRAMONO_UNINITVARIABLES_H_
