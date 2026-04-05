#pragma once

#include "llvm/Pass.h"

#include "Analysis/SymbolicExecution/AnalysisDriver.h"

namespace llvm {

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
