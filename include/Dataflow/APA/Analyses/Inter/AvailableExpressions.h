#pragma once

#include "llvm/IR/Function.h"
#include "llvm/IR/Instruction.h"

#include "Dataflow/APA/APA.h"
#include "Dataflow/APA/Domains/AvailableExpressionsDomain.h"

namespace elimination {

constexpr unsigned kDefaultInterElimAvailableExpressionsCallStringLength = 2;

using InterAvailableExpressionsResult =
    InterDataFlowResultT<kDefaultInterElimAvailableExpressionsCallStringLength,
                         AvailableExpressionsFact, llvm::Instruction *>;

InterAvailableExpressionsResult runInterElimAvailableExpressions(
    llvm::Function *Entry,
    const dataflow::controlflow::InterCFG *ICF = nullptr,
    EliminationOptions Options = defaultContextOptions());

InterAvailableExpressionsResult runInterSummaryElimAvailableExpressions(
    llvm::Function *Entry, const dataflow::controlflow::InterCFG *ICF = nullptr,
    PathSummaryEquationOptions Options = {});

} // namespace elimination
