#pragma once

#include "Alias/InclusionBased/TPA/PointerAnalysis/MemoryModel/Pointer.h"

#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace llvm {
class ConstantPointerNull;
class Value;
class UndefValue;
} // namespace llvm

namespace tpa {

// Pointer manager for SSA-style pointer representation
//
// Manages Pointer objects which represent the pair (context, LLVM value).
// This enables context-sensitive pointer analysis by distinguishing the same
// LLVM value appearing in different calling contexts.
//
// Pointer Identity:
// - A Pointer is uniquely identified by (context, value)
// - The same LLVM value in different contexts is a different Pointer
// - This is similar to SSA form where variables have unique versions
//
// Special Pointers:
// - Universal pointer: Represents "may point to anything"
// - Null pointer: Represents the null constant
// Both use the global context
//
// Design:
// - Flyweight pattern: Pointers are interned in ptrSet
// - valuePtrMap: Reverse mapping from LLVM value to its Pointers
// - Enables efficient lookup and deduplication
class PointerManager {
private:
  // Set of all created pointers (flyweight pattern)
  std::unordered_set<Pointer> ptrSet;

  // Special pointers
  // uPtr: Points to everything (value is UndefValue)
  // nPtr: Points to null (value is ConstantPointerNull)
  const Pointer *uPtr;
  const Pointer *nPtr;

  // Reverse mapping: LLVM value -> all Pointers with this value
  // Useful for context-insensitive queries
  using PointerVector = std::vector<const Pointer *>;
  std::unordered_map<const llvm::Value *, PointerVector> valuePtrMap;

  // Create a new Pointer (internal method)
  const Pointer *buildPointer(const context::Context *ctx,
                              const llvm::Value *val);

public:
  PointerManager();

  // Set up the universal pointer
  const Pointer *setUniversalPointer(const llvm::UndefValue *);
  // Get the universal pointer
  const Pointer *getUniversalPointer() const;
  // Set up the null pointer
  const Pointer *setNullPointer(const llvm::ConstantPointerNull *);
  // Get the null pointer
  const Pointer *getNullPointer() const;

  // Get or create a Pointer for (context, value)
  // If the pointer doesn't exist, creates it
  // Returns the unique Pointer for this (context, value) pair
  const Pointer *getOrCreatePointer(const context::Context *ctx,
                                    const llvm::Value *val);
  // Get the Pointer for (context, value) if it exists
  // Returns nullptr if not found (doesn't create)
  const Pointer *getPointer(const context::Context *ctx,
                            const llvm::Value *val) const;
  // Get all Pointers with a given LLVM value (across all contexts)
  // Returns empty vector if no pointers exist for this value
  PointerVector getPointersWithValue(const llvm::Value *val) const;
};

} // namespace tpa