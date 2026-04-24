#pragma once

#include "llvm/IR/Module.h"
#include "llvm/Passes/PassBuilder.h"

using namespace llvm;

namespace transform {

struct GlobalCleanup : public PassInfoMixin<GlobalCleanup> {

  PreservedAnalyses run(Module &M, ModuleAnalysisManager &analysisManager);
  static llvm::StringRef name() { return "GlobalCleanup"; }
};

struct ResolveAliases : public PassInfoMixin<ResolveAliases> {

  PreservedAnalyses run(Module &M, ModuleAnalysisManager &analysisManager);
  static llvm::StringRef name() { return "ResolveAliases"; }
};

} // namespace transform
