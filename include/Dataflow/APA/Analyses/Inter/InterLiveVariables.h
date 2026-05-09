#ifndef DATAFLOW_APA_CLIENTS_LLVM_INTER_LIVEVARIABLES_H_
#define DATAFLOW_APA_CLIENTS_LLVM_INTER_LIVEVARIABLES_H_

#include "llvm/IR/Function.h"
#include "llvm/IR/Instruction.h"

#include "Dataflow/APA/APA.h"

#include <set>

namespace elimination {

constexpr unsigned kDefaultInterElimLiveVariablesCallStringLength = 2;

using LiveVariablesFact = std::set<const llvm::Value *>;
using InterLiveVariablesResult = InterDataFlowResultT<
    kDefaultInterElimLiveVariablesCallStringLength, LiveVariablesFact,
    llvm::Instruction *>;

InterLiveVariablesResult
runInterElimLiveVariables(llvm::Function *Entry,
                          const dataflow::controlflow::InterCFG *ICF = nullptr);

} // namespace elimination

#endif // DATAFLOW_APA_CLIENTS_LLVM_INTER_LIVEVARIABLES_H_
