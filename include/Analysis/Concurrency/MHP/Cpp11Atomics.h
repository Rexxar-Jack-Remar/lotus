/**
 * @file Cpp11Atomics.h
 * @brief C++11 Atomics Recognition
 *
 * This file provides helpers to identify C++11 atomic operations
 * and their memory ordering in LLVM IR.
 *
 * @author rainoftime
 * @date 2025-2026
 */

#ifndef CPP11_ATOMICS_H
#define CPP11_ATOMICS_H

#include <llvm/IR/Instruction.h>
#include <llvm/IR/Value.h>

namespace Cpp11Atomics {

// Enum to represent C++11 memory orderings
enum class MemoryOrder {
  NotAtomic,
  Relaxed,
  Consume,
  Acquire,
  Release,
  AcquireRelease,
  SequentiallyConsistent
};

// Functions to identify atomic operations
bool isAtomic(const llvm::Instruction *inst);
MemoryOrder getMemoryOrder(const llvm::Instruction *inst);
const llvm::Value *getAtomicPointer(const llvm::Instruction *inst);

// Functions to check for specific properties
bool isLockFree(const llvm::Instruction *inst);
bool isStore(const llvm::Instruction *inst);
bool isLoad(const llvm::Instruction *inst);

} // namespace Cpp11Atomics

#endif // CPP11_ATOMICS_H
