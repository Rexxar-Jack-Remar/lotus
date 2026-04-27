#pragma once

#include "llvm/Pass.h"

namespace llvm {
class Module;
} // namespace llvm

namespace lotus {
namespace analysis {
namespace purity {

class PurityAttrInferencePass : public llvm::ModulePass {
public:
  static char ID;

  PurityAttrInferencePass() : llvm::ModulePass(ID) {}

  bool runOnModule(llvm::Module &module) override;

  void getAnalysisUsage(llvm::AnalysisUsage &AU) const override {
    AU.setPreservesCFG();
  }
};

llvm::Pass *createPurityAttrInferencePass();

} // namespace purity
} // namespace analysis
} // namespace lotus
