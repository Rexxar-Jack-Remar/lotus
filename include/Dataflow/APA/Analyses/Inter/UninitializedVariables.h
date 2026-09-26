#pragma once

#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/AssumptionCache.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Value.h"

#include "Dataflow/APA/APA.h"
#include "Dataflow/APA/Domains/UninitializedVariablesDomain.h"

namespace elimination {

constexpr unsigned kDefaultInterElimUninitializedVariablesCallStringLength = 2;

using InterUninitializedVariablesResult = InterDataFlowResultT<
    kDefaultInterElimUninitializedVariablesCallStringLength,
    UninitializedVariablesFact, llvm::Instruction *>;

InterUninitializedVariablesResult runInterElimUninitializedVariables(
    llvm::Function *Entry, llvm::AAResults *AA = nullptr,
    llvm::AssumptionCache *AC = nullptr, llvm::DominatorTree *DT = nullptr,
    const dataflow::controlflow::InterCFG *ICF = nullptr,
    EliminationOptions Options = defaultContextOptions());

InterUninitializedVariablesResult runInterSummaryElimUninitializedVariables(
    llvm::Function *Entry, llvm::AAResults *AA = nullptr,
    llvm::AssumptionCache *AC = nullptr, llvm::DominatorTree *DT = nullptr,
    const dataflow::controlflow::InterCFG *ICF = nullptr,
    PathSummaryEquationOptions Options = {});

} // namespace elimination
