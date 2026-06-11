/** @file SymbolicExecutionWrapper.h @brief LLVM pass wrapper for symbolic execution analysis. */
#pragma once

#include "llvm/Pass.h"

#include "Analysis/SymbolicExecution/AnalysisDriver.h"

namespace llvm {

/// LLVM module pass entry point for the SymbolicExecution subsystem.
///
/// The wrapper requests the analyses needed by the symbolic execution engine,
/// runs the AnalysisDriver over the module, and converts collected traces into
/// Lotus bug reports.
class SymbolicExecutionWrapper : public ModulePass {
public:
  static char ID;
  SymbolicExecutionWrapper();
  ~SymbolicExecutionWrapper() override;

  void getAnalysisUsage(AnalysisUsage &AU) const override;
  bool runOnModule(Module &M) override;

private:
  static int
  getBugTypeId(SymbolicExecution::AnalysisState::SymexBugType bug_type);
  void emitBugReports(const SymbolicExecution::AnalysisDriver &driver) const;
};

} // namespace llvm
