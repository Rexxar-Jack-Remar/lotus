#pragma once

#include "llvm/IR/Module.h"
#include "llvm/Passes/PassBuilder.h"

using namespace llvm;

namespace transform {

struct ExpandByValPass : public PassInfoMixin<ExpandByValPass> {

  PreservedAnalyses run(Module &M, ModuleAnalysisManager &analysisManager);
  static llvm::StringRef name() { return "ExpandByValPass"; }
};

} // namespace transform
