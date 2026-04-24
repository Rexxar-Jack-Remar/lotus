#pragma once

#include "llvm/IR/Function.h"
#include "llvm/Passes/PassBuilder.h"

using namespace llvm;

namespace transform {

struct ExpandIndirectBr : public PassInfoMixin<ExpandIndirectBr> {

  PreservedAnalyses run(Module &M, ModuleAnalysisManager &analysisManager);
  static llvm::StringRef name() { return "ExpandIndirectBr"; }
};

} // namespace transform
