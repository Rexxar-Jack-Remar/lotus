//
// Created by peiming on 2/10/20.
//

#pragma once

#include <llvm/Pass.h>

class WrapperFunIdentifyPass : public llvm::ModulePass {

public:
  static char ID;
  explicit WrapperFunIdentifyPass() : ModulePass(ID) {}

  bool runOnModule(llvm::Module &M) override;
};

