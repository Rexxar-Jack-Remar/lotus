#pragma once

#include <llvm/IR/PassManager.h>

/* The CastElim pass eliminates some unnecessary casts that can
 * complicate later analyses. */
class CastElimPass final : public llvm::PassInfoMixin<CastElimPass> {
public:
  llvm::PreservedAnalyses run(llvm::Function &F,
                              llvm::FunctionAnalysisManager &FAM);
};

