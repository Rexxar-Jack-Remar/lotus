//
// Created by peiming on 11/13/19.
//
#pragma once

#include <llvm/IR/Function.h>

namespace aser {

// TODO: think about this!

class DefaultExtFunctions {
private:
  const static std::set<llvm::StringRef> THREAD_CREATIONS;

public:
  inline static bool isThreadCreation(const llvm::Function *fun) {
    return THREAD_CREATIONS.find(fun->getName()) != THREAD_CREATIONS.end();
  }
};

} // namespace aser
