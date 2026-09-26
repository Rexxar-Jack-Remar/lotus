//
// Created by peiming on 3/14/20.
//

#pragma once

#include <llvm/Pass.h>

namespace aser {

class InsertGlobalCtorCallPass : public llvm::ModulePass {
public:
  static char ID;
  explicit InsertGlobalCtorCallPass() : llvm::ModulePass(ID) {}

  bool runOnModule(llvm::Module &M) override;
};

} // namespace aser

