#pragma once

#include <llvm/Pass.h>

namespace lotus {

class UniqueIRMarkerPass : public llvm::ModulePass {
public:
  static char ID;

  UniqueIRMarkerPass();

  bool runOnModule(llvm::Module &module) override;
  void getAnalysisUsage(llvm::AnalysisUsage &analysis_usage) const override;
};

} // namespace lotus
