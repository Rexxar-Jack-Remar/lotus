#pragma once

#include "Alias/Specialized/DFPA/Config.h"
#include "Alias/Specialized/DFPA/Result.h"

#include <llvm/Pass.h>

namespace llvm {
class Module;
} // namespace llvm

namespace dfpa {

class DFPAPass : public llvm::ModulePass {
public:
  static char ID;

  explicit DFPAPass(DFPAConfig Config = DFPAConfig());
  ~DFPAPass() override = default;

  void getAnalysisUsage(llvm::AnalysisUsage &AU) const override;
  bool runOnModule(llvm::Module &M) override;

  const DFPAResult &getResult() const { return result_; }
  const DFPAConfig &getConfig() const { return config_; }

private:
  DFPAConfig config_;
  DFPAResult result_;
};

} // namespace dfpa
