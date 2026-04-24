#pragma once

#include "llvm/IR/Function.h"
#include "llvm/Passes/PassBuilder.h"

using namespace llvm;

namespace transform {

struct ExpandGetElementPtrPass : public PassInfoMixin<ExpandGetElementPtrPass> {

  PreservedAnalyses run(Function &F, FunctionAnalysisManager &analysisManager);
  static llvm::StringRef name() { return "ExpandGetElementPtrPass"; }
};

} // namespace transform
