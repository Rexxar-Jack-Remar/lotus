//
// Created by peiming on 1/10/20.
//

#pragma once

#include <llvm/Pass.h>

namespace aser {

class RemoveExceptionHandlerPass : public llvm::FunctionPass {
public:
  static char ID;
  RemoveExceptionHandlerPass() : llvm::FunctionPass(ID) {}

  bool runOnFunction(llvm::Function &F) override;
  bool doInitialization(llvm::Module &M) override;
};

} // namespace aser

