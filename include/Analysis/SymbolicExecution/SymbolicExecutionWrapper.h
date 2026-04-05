#pragma once

#include "llvm/Pass.h"

#include "Analysis/SymbolicExecution/AnalysisDriver.h"
#include "IR/GVFG/GuardedValueFlowGraph.h"

namespace llvm {

class SymbolicExecutionWrapper : public ModulePass {
public:
  static char ID;
  SymbolicExecutionWrapper();
  ~SymbolicExecutionWrapper() override;

  void getAnalysisUsage(AnalysisUsage &AU) const override;
  bool runOnModule(Module &M) override;
};

} // namespace llvm
