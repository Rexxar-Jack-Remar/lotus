/*
 * LotusAA - Type Definitions and Utilities
 * 
 * Common types, type aliases, and comparators used throughout LotusAA.
 * Provides LLVM-compatible data structures and helper types.
 */

#pragma once

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Value.h>
#include <cstdint>
#include <map>
#include <vector>

namespace llvm {

using path_cond_t = Value *;

// LLVM value comparator for map/set ordering
struct llvm_cmp {
  bool operator()(const Value *A, const Value *B) const {
    return A < B;
  }
  
  bool operator()(const BasicBlock *A, const BasicBlock *B) const {
    return A < B;
  }
  
  bool operator()(const Function *A, const Function *B) const {
    return A < B;
  }
};

// Singleton for consistent value indexing
class LLVMValueIndex {
  static LLVMValueIndex *Instance;
  LLVMValueIndex() {}
  
public:
  static LLVMValueIndex *get() {
    if (!Instance)
      Instance = new LLVMValueIndex();
    return Instance;
  }
};

} // namespace llvm
