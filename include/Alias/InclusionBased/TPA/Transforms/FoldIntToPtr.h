#pragma once

#include "llvm/IR/Function.h"
#include "llvm/Passes/PassBuilder.h"

using namespace llvm;

namespace transform {

struct FoldIntToPtrPass : public PassInfoMixin<FoldIntToPtrPass> {

  PreservedAnalyses run(Function &F, FunctionAnalysisManager &analysisManager);
  static llvm::StringRef name() { return "FoldIntToPtrPass"; }
};

} // namespace transform
