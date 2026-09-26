#pragma once

#include "llvm/IR/Function.h"
#include "llvm/IR/Instruction.h"

#include "Dataflow/APA/APA.h"
#include "Dataflow/APA/Domains/SignDomain.h"

namespace elimination {

constexpr unsigned kDefaultInterElimSignCallStringLength = 2;

using InterSignResult =
    InterDataFlowResultT<kDefaultInterElimSignCallStringLength, SignMap,
                         llvm::Instruction *>;

InterSignResult
runInterElimSign(llvm::Function *Entry,
                 const dataflow::controlflow::InterCFG *ICF = nullptr,
    EliminationOptions Options = defaultContextOptions());

InterSignResult
runInterSummaryElimSign(llvm::Function *Entry,
                        const dataflow::controlflow::InterCFG *ICF = nullptr,
                        PathSummaryEquationOptions Options = {});

} // namespace elimination
