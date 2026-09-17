#pragma once

#include "llvm/Analysis/AssumptionCache.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instruction.h"

#include "Dataflow/APA/APA.h"
#include "Dataflow/APA/Domains/NonNullDomain.h"

namespace elimination {

constexpr unsigned kDefaultInterElimNonNullCallStringLength = 2;

using InterNonNullResult =
    InterDataFlowResultT<kDefaultInterElimNonNullCallStringLength, NonNullFact,
                         NonNullEdgeTransfer>;

InterNonNullResult
runInterElimNonNull(llvm::Function *Entry, llvm::AssumptionCache *AC = nullptr,
                    llvm::DominatorTree *DT = nullptr,
                    const dataflow::controlflow::InterCFG *ICF = nullptr,
    EliminationOptions Options = defaultContextOptions());

InterNonNullResult
runInterSummaryElimNonNull(llvm::Function *Entry,
                           llvm::AssumptionCache *AC = nullptr,
                           llvm::DominatorTree *DT = nullptr,
                           const dataflow::controlflow::InterCFG *ICF = nullptr,
                           PathSummaryEquationOptions Options = {});

} // namespace elimination
