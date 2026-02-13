#ifndef DATAFLOW_ELIMINATION_ANALYSES_INTRA_INTRAREACHABLE_H_
#define DATAFLOW_ELIMINATION_ANALYSES_INTRA_INTRAREACHABLE_H_

#include "Dataflow/Elimination/DataFlow.h"
#include "Dataflow/Elimination/LLVM/LLVMEliminationProblem.h"

#include "llvm/IR/Function.h"
#include "llvm/IR/Instruction.h"

namespace elimination {

using ReachableFact = bool;
using ReachableResult = DataFlowResultT<llvm::Instruction *, ReachableFact,
                                        llvm::Instruction *>;

ReachableResult runIntraElimReachable(llvm::Function *F,
                                      EliminationOptions Opts = {});

} // namespace elimination

#endif // DATAFLOW_ELIMINATION_ANALYSES_INTRA_INTRAREACHABLE_H_
