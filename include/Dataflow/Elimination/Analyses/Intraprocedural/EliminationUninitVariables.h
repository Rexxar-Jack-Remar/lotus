#ifndef DATAFLOW_ELIMINATION_ANALYSES_INTRAPROCEDURAL_ELIMINATIONUNINITVARIABLES_H_
#define DATAFLOW_ELIMINATION_ANALYSES_INTRAPROCEDURAL_ELIMINATIONUNINITVARIABLES_H_

#include "Dataflow/Elimination/DataFlow.h"
#include "Dataflow/Elimination/LLVM/LLVMEliminationProblem.h"

#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/AssumptionCache.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Value.h"

#include <set>

namespace elimination {

using UninitVariablesFact = std::set<llvm::Value *>;
using UninitVariablesResult = DataFlowResultT<llvm::Instruction *,
                                              UninitVariablesFact,
                                              llvm::Instruction *>;

UninitVariablesResult runIntraElimUninitVariables(llvm::Function *F,
                                                 EliminationOptions Opts = {});

UninitVariablesResult runIntraElimUninitVariables(llvm::Function *F,
                                                  llvm::AAResults *AA,
                                                  EliminationOptions Opts = {});

UninitVariablesResult runIntraElimUninitVariables(llvm::Function *F,
                                                  llvm::AAResults *AA,
                                                  llvm::AssumptionCache *AC,
                                                  llvm::DominatorTree *DT,
                                                  EliminationOptions Opts = {});

} // namespace elimination

#endif // DATAFLOW_ELIMINATION_ANALYSES_INTRAPROCEDURAL_ELIMINATIONUNINITVARIABLES_H_
