#pragma once

#include "llvm/IR/Function.h"
#include "llvm/IR/Instruction.h"

#include "Dataflow/APA/APA.h"
#include "Dataflow/APA/Domains/LocksetDomain.h"

namespace elimination {

constexpr unsigned kDefaultInterElimLocksetCallStringLength = 2;

using InterLocksetResult =
    InterDataFlowResultT<kDefaultInterElimLocksetCallStringLength, LocksetFact,
                         llvm::Instruction *>;

InterLocksetResult
runInterElimLockset(llvm::Function *Entry,
                    const dataflow::controlflow::InterCFG *ICF = nullptr,
    EliminationOptions Options = defaultContextOptions());

InterLocksetResult
runInterSummaryElimLockset(llvm::Function *Entry,
                           const dataflow::controlflow::InterCFG *ICF = nullptr,
                           PathSummaryEquationOptions Options = {});

} // namespace elimination
