#ifndef DATAFLOW_ELIMINATION_ANALYSES_INTRA_INTRALIVEVARIABLES_H_
#define DATAFLOW_ELIMINATION_ANALYSES_INTRA_INTRALIVEVARIABLES_H_

#include "Dataflow/Elimination/DataFlow.h"

#include "llvm/IR/Function.h"
#include "llvm/IR/Instruction.h"

#include <set>

namespace elimination {

using LiveVariablesFact = std::set<const llvm::Value *>;
using LiveVariablesResult =
    DataFlowResultT<llvm::Instruction *, LiveVariablesFact,
                    llvm::Instruction *>;

LiveVariablesResult runIntraElimLiveVariables(llvm::Function *F,
                                              EliminationOptions Opts = {});

} // namespace elimination

#endif // DATAFLOW_ELIMINATION_ANALYSES_INTRA_INTRALIVEVARIABLES_H_
