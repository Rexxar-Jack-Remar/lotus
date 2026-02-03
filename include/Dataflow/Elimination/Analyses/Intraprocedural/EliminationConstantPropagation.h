#ifndef DATAFLOW_ELIMINATION_ANALYSES_INTRAPROCEDURAL_ELIMINATIONCONSTANTPROPAGATION_H_
#define DATAFLOW_ELIMINATION_ANALYSES_INTRAPROCEDURAL_ELIMINATIONCONSTANTPROPAGATION_H_

#include "Dataflow/Elimination/DataFlow.h"
#include "Dataflow/Elimination/LLVM/LLVMEliminationProblem.h"

#include "llvm/IR/Function.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Value.h"

#include <cstdint>
#include <unordered_map>

namespace elimination {

enum class ConstantPropagationTag {
  Top,
  Const,
  Bottom,
};

struct ConstantPropagationValue {
  ConstantPropagationTag Tag = ConstantPropagationTag::Top;
  int64_t ConstValue = 0;

  bool operator==(const ConstantPropagationValue &Other) const {
    return Tag == Other.Tag && ConstValue == Other.ConstValue;
  }
};

using ConstantPropagationMap =
    std::unordered_map<const llvm::Value *, ConstantPropagationValue>;

using ConstantPropagationResult =
    DataFlowResultT<llvm::Instruction *, ConstantPropagationMap,
                    llvm::Instruction *>;

ConstantPropagationResult
runIntraElimConstantPropagation(llvm::Function *F,
                                EliminationOptions Opts = {});

} // namespace elimination

#endif // DATAFLOW_ELIMINATION_ANALYSES_INTRAPROCEDURAL_ELIMINATIONCONSTANTPROPAGATION_H_
