//
// Created by peiming on 3/9/20.
//

#pragma once

#include <llvm/Pass.h>

class StandardHeapAPIRewritePass : public llvm::ModulePass {

public:
  static char ID;
  explicit StandardHeapAPIRewritePass() : ModulePass(ID) {}

  bool runOnModule(llvm::Module &M) override;
};

