#ifndef DATAFLOW_ELIMINATION_ANALYSES_INTRA_INTRANONNULL_H_
#define DATAFLOW_ELIMINATION_ANALYSES_INTRA_INTRANONNULL_H_

#include "Dataflow/Elimination/DataFlow.h"

#include "llvm/Analysis/AssumptionCache.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instruction.h"

#include <set>

namespace elimination {

using NonNullFact = std::set<const llvm::Value *>;
struct NonNullEdgeTransfer {
  llvm::Instruction *Src = nullptr;
  llvm::Instruction *Dst = nullptr;
};

using NonNullResult =
    DataFlowResultT<llvm::Instruction *, NonNullFact, NonNullEdgeTransfer>;

NonNullResult runIntraElimNonNull(llvm::Function *F,
                                  EliminationOptions Opts = {});

NonNullResult runIntraElimNonNull(llvm::Function *F,
                                  llvm::AssumptionCache *AC,
                                  llvm::DominatorTree *DT,
                                  EliminationOptions Opts = {});

} // namespace elimination

#endif // DATAFLOW_ELIMINATION_ANALYSES_INTRA_INTRANONNULL_H_
