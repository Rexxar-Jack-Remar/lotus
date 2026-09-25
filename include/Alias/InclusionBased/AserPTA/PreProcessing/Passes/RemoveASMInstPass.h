//
// Created by peiming on 4/15/20.
//

#pragma once

#include <llvm/Pass.h>

namespace aser {

class RemoveASMInstPass : public llvm::FunctionPass {
public:
  static char ID;
  explicit RemoveASMInstPass() : llvm::FunctionPass(ID) {}

  bool runOnFunction(llvm::Function &F) override;
};

} // namespace aser

