#ifndef DATAFLOW_ELIMINATION_ANALYSES_INTRA_INTRAREACHINGDEFINITIONS_H_
#define DATAFLOW_ELIMINATION_ANALYSES_INTRA_INTRAREACHINGDEFINITIONS_H_

#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/MemorySSA.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instruction.h"

#include "Dataflow/APA/DataFlow.h"
#include "Dataflow/APA/LLVM/LLVMEliminationProblem.h"

#include <set>

namespace elimination {

using ReachingDefinitionsFact = std::set<const llvm::Value *>;
using ReachingDefinitionsResult =
    DataFlowResultT<llvm::Instruction *, ReachingDefinitionsFact,
                    llvm::Instruction *>;

ReachingDefinitionsResult
runIntraElimReachingDefinitions(llvm::Function *F,
                                EliminationOptions Opts = {});

ReachingDefinitionsResult
runIntraElimReachingDefinitions(llvm::Function *F, llvm::AAResults *AA,
                                EliminationOptions Opts = {});

ReachingDefinitionsResult
runIntraElimReachingDefinitions(llvm::Function *F, llvm::AAResults *AA,
                                llvm::MemorySSA *MSSA,
                                EliminationOptions Opts = {});

} // namespace elimination

#endif // DATAFLOW_ELIMINATION_ANALYSES_INTRA_INTRAREACHINGDEFINITIONS_H_
