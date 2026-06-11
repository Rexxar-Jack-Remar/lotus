/** @file InstResolver.h @brief Instruction resolver for symbolic execution of LLVM IR. */
#pragma once

#include <string>

#include <llvm/IR/Value.h>
#include <llvm/Support/raw_ostream.h>

namespace llvm {

class InstResolver {
public:
  std::string restore_value_expr(Value *V) const {
    if (!V)
      return "<null>";
    if (V->hasName())
      return V->getName().str();
    std::string buffer;
    raw_string_ostream os(buffer);
    V->printAsOperand(os, false);
    return os.str();
  }

  std::string restore_access_path_expr(Value *V, bool) const {
    return restore_value_expr(V);
  }
};

} // namespace llvm
