#ifndef DATAFLOW_ELIMINATION_ANALYSES_INTRA_AVAILABLEEXPRESSIONS_H_
#define DATAFLOW_ELIMINATION_ANALYSES_INTRA_AVAILABLEEXPRESSIONS_H_

#include "Dataflow/Elimination/DataFlow.h"
#include "Dataflow/Elimination/Analyses/Intra/IntraExpressionKey.h"
#include "Dataflow/Elimination/LLVM/LLVMEliminationProblem.h"

#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/MemorySSA.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instruction.h"

#include <set>

namespace elimination {

using AvailableExpressionsFact = std::set<ExpressionKey>;
using AvailableExpressionsResult =
    DataFlowResultT<llvm::Instruction *, AvailableExpressionsFact,
                    llvm::Instruction *>;

AvailableExpressionsResult
runIntraElimAvailableExpressions(llvm::Function *F,
                                 EliminationOptions Opts = {});

AvailableExpressionsResult
runIntraElimAvailableExpressions(llvm::Function *F, llvm::AAResults *AA,
                                 EliminationOptions Opts = {});

AvailableExpressionsResult
runIntraElimAvailableExpressions(llvm::Function *F, llvm::AAResults *AA,
                                 llvm::DominatorTree *DT,
                                 llvm::TargetLibraryInfo *TLI,
                                 EliminationOptions Opts = {});

AvailableExpressionsResult
runIntraElimAvailableExpressions(llvm::Function *F, llvm::AAResults *AA,
                                 llvm::DominatorTree *DT,
                                 llvm::TargetLibraryInfo *TLI,
                                 llvm::MemorySSA *MSSA,
                                 EliminationOptions Opts = {});

} // namespace elimination

#endif // DATAFLOW_ELIMINATION_ANALYSES_INTRA_INTRAAVAILABLEEXPRESSIONS_H_
