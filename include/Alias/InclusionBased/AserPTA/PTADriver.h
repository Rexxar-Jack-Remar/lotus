//
// PTADriver.h
//
// Driver code for running pointer analysis passes, including PTADriverPass
// and runAnalysis function.
//

#ifndef ASER_PTA_PTADRIVER_H
#define ASER_PTA_PTADRIVER_H

#include "Alias/InclusionBased/AserPTA/PointerAnalysis/PointerAnalysisPass.h"
#include "Alias/InclusionBased/AserPTA/PreProcessing/Passes/CanonicalizeGEPPass.h"
#include "Alias/InclusionBased/AserPTA/PreProcessing/Passes/LoweringMemCpyPass.h"
#include "Alias/InclusionBased/AserPTA/PreProcessing/Passes/RemoveASMInstPass.h"
#include "Alias/InclusionBased/AserPTA/PreProcessing/Passes/RemoveExceptionHandlerPass.h"
#include "Alias/InclusionBased/AserPTA/PreProcessing/Passes/StandardHeapAPIRewritePass.h"

#include <llvm/ADT/Statistic.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/Pass.h>

namespace aser {

/// @brief LLVM module pass that drives pointer analysis execution.
/// @tparam PTASolver The pointer analysis solver type to use.
template <typename PTASolver> class PTADriverPass : public llvm::ModulePass {
private:
  bool dumpStats;

public:
  static char ID;
  PTADriverPass(bool dumpStats = true) : ModulePass(ID), dumpStats(dumpStats) {}

  void getAnalysisUsage(llvm::AnalysisUsage &AU) const override {
    AU.addRequired<PointerAnalysisPass<PTASolver>>();
  }

  bool runOnModule(llvm::Module &M) override {
    if (dumpStats) {
      llvm::ResetStatistics();
    }

    getAnalysis<PointerAnalysisPass<PTASolver>>().analyze(&M);

    if (dumpStats) {
      llvm::PrintStatistics(llvm::outs());
      llvm::ResetStatistics();
    }

    getAnalysis<PointerAnalysisPass<PTASolver>>().release();
    return false;
  }
};

template <typename PTASolver> char PTADriverPass<PTASolver>::ID = 0;

/// @brief Runs pointer analysis on a module with preprocessing passes.
/// @tparam Solver The pointer analysis solver type to use.
/// @param M The LLVM module to analyze.
/// @param dumpStats Whether to dump analysis statistics.
template <typename Solver>
void runAnalysis(llvm::Module &M, bool dumpStats = true) {
  llvm::legacy::PassManager passes;

  // Preprocessing passes
  llvm::errs() << "Preprocessing IR...\n";
  passes.add(new CanonicalizeGEPPass());
  passes.add(new LoweringMemCpyPass());
  passes.add(new RemoveExceptionHandlerPass());
  passes.add(new RemoveASMInstPass());
  passes.add(new StandardHeapAPIRewritePass());

  // Analysis passes
  auto *ptaPass = new PointerAnalysisPass<Solver>();
  passes.add(ptaPass);
  passes.add(new PTADriverPass<Solver>(dumpStats));

  llvm::errs() << "Running pointer analysis...\n";
  passes.run(M);
  llvm::errs() << "Analysis completed.\n";
}

} // namespace aser

#endif // ASER_PTA_PTADRIVER_H
