#pragma once

#include "llvm/IR/Module.h"
#include "llvm/Passes/PassBuilder.h"

using namespace llvm;

namespace transform {

struct LowerGlobalCtorPass : public PassInfoMixin<LowerGlobalCtorPass> {

  PreservedAnalyses run(Module &M, ModuleAnalysisManager &analysisManager);
  static llvm::StringRef name() { return "LowerGlobalCtorPass"; }
};

} // namespace transform
