#ifndef DATAFLOW_ELIMINATION_ANALYSES_INTRA_INTRAVERYBUSYEXPRESSIONS_H_
#define DATAFLOW_ELIMINATION_ANALYSES_INTRA_INTRAVERYBUSYEXPRESSIONS_H_

#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/MemorySSA.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instruction.h"

#include "Dataflow/APA/Support/ExpressionKey.h"
#include "Dataflow/APA/DataFlow.h"

#include <set>

namespace elimination {

using VeryBusyExpressionsFact = std::set<ExpressionKey>;
using VeryBusyExpressionsResult =
    DataFlowResultT<llvm::Instruction *, VeryBusyExpressionsFact,
                    llvm::Instruction *>;

VeryBusyExpressionsResult
runIntraElimVeryBusyExpressions(llvm::Function *F,
                                EliminationOptions Opts = {});

VeryBusyExpressionsResult
runIntraElimVeryBusyExpressions(llvm::Function *F, llvm::AAResults *AA,
                                EliminationOptions Opts = {});

VeryBusyExpressionsResult runIntraElimVeryBusyExpressions(
    llvm::Function *F, llvm::AAResults *AA, llvm::DominatorTree *DT,
    llvm::TargetLibraryInfo *TLI, EliminationOptions Opts = {});

VeryBusyExpressionsResult runIntraElimVeryBusyExpressions(
    llvm::Function *F, llvm::AAResults *AA, llvm::DominatorTree *DT,
    llvm::TargetLibraryInfo *TLI, llvm::MemorySSA *MSSA,
    EliminationOptions Opts = {});

} // namespace elimination

#endif // DATAFLOW_ELIMINATION_ANALYSES_INTRA_INTRAVERYBUSYEXPRESSIONS_H_
