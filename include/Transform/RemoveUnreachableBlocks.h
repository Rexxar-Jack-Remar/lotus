#ifndef TRANSFORM_REMOVEUNREACHABLEBLOCKS_H
#define TRANSFORM_REMOVEUNREACHABLEBLOCKS_H

#include "llvm/IR/Function.h"
#include "llvm/IR/PassManager.h"

struct RemoveUnreachableBlocksPass
    : llvm::PassInfoMixin<RemoveUnreachableBlocksPass> {
  llvm::PreservedAnalyses run(llvm::Function &F,
                              llvm::FunctionAnalysisManager &);
};

#endif // TRANSFORM_REMOVEUNREACHABLEBLOCKS_H
