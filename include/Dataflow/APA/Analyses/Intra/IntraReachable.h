#ifndef DATAFLOW_ELIMINATION_ANALYSES_INTRA_INTRAREACHABLE_H_
#define DATAFLOW_ELIMINATION_ANALYSES_INTRA_INTRAREACHABLE_H_

#include "llvm/IR/Function.h"
#include "llvm/IR/Instruction.h"

#include "Dataflow/APA/DataFlow.h"
#include "Dataflow/APA/LLVM/LLVMEliminationProblem.h"

namespace elimination {

using ReachableFact = bool;
using ReachableResult =
    DataFlowResultT<llvm::Instruction *, ReachableFact, llvm::Instruction *>;

ReachableResult runIntraElimReachable(llvm::Function *F,
                                      EliminationOptions Opts = {});

} // namespace elimination

#endif // DATAFLOW_ELIMINATION_ANALYSES_INTRA_INTRAREACHABLE_H_
