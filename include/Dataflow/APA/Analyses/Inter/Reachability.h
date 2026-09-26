#pragma once

#include "llvm/IR/Function.h"
#include "llvm/IR/Instruction.h"

#include "Dataflow/APA/APA.h"
#include "Dataflow/APA/Domains/ReachabilityDomain.h"

namespace elimination {

constexpr unsigned kDefaultInterElimReachabilityCallStringLength = 2;

using InterReachabilityResult =
    InterDataFlowResultT<kDefaultInterElimReachabilityCallStringLength,
                         ReachableFact, llvm::Instruction *>;

InterReachabilityResult
runInterElimReachability(llvm::Function *Entry,
                         const dataflow::controlflow::InterCFG *ICF = nullptr,
    EliminationOptions Options = defaultContextOptions());

InterReachabilityResult runInterSummaryElimReachability(
    llvm::Function *Entry, const dataflow::controlflow::InterCFG *ICF = nullptr,
    PathSummaryEquationOptions Options = {});

// Modular (E6) variant: builds per-procedure summaries and interprets them with
// a context-insensitive (functional) fixpoint. Opt-in alternative to the
// whole-program ForwardInterSummarySolver above.
InterReachabilityResult runModularInterReachability(
    llvm::Function *Entry, const dataflow::controlflow::InterCFG *ICF = nullptr,
    PathSummaryEquationOptions Options = {});

} // namespace elimination
