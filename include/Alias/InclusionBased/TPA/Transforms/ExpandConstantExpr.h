#pragma once

#include "llvm/IR/Function.h"
#include "llvm/Passes/PassBuilder.h"

using namespace llvm;

namespace transform {

struct ExpandConstantExprPass : public PassInfoMixin<ExpandConstantExprPass> {

  PreservedAnalyses run(Function &F, FunctionAnalysisManager &analysisManager);
  static llvm::StringRef name() { return "ExpandConstantExprPass"; }
};

} // namespace transform
