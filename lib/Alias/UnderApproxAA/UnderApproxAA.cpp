/**
 * @file UnderApproxAA.cpp
 * @brief Implementation of under-approximation alias analysis
 *
 * This file implements the LLVM AAResult interface for under-approximation
 * alias analysis. The analysis uses union-find with congruence closure to
 * identify must-alias relationships within functions.
 *
 * Key design decisions:
 * - Per-function caching: Each function's EquivDB is built once and reused
 * - Intra-procedural only: Cross-function queries return NoAlias (conservative)
 * - Sound under-approximation: Returns MustAlias only when guaranteed
 * - Optional MemorySSA: Store-load forwarding for more precision (sound)
 * - Optional DominatorTree: Single-store alloca forwarding for more precision
 *   (sound)
 */

#include "Alias/UnderApproxAA/UnderApproxAA.h"

#include "Alias/UnderApproxAA/EquivDB.h"

#include <unordered_map>

#include <llvm/Analysis/MemoryLocation.h>
#include <llvm/IR/Instructions.h>

using namespace llvm;
using namespace UnderApprox;

// ---------------------------------------------------------------------------
// Per-function cache – built lazily, reused by all subsequent queries
// ---------------------------------------------------------------------------

namespace {
/// Cache key: combines function pointer with analysis providers
/// This ensures different provider configurations get different caches
struct CacheKey {
  const Function *F;
  UnderApproxAA::MemorySSAProvider MSSAProv;
  UnderApproxAA::DominatorTreeProvider DTProv;

  bool operator==(const CacheKey &Other) const {
    return F == Other.F && MSSAProv == Other.MSSAProv && DTProv == Other.DTProv;
  }
};

/// Hash function for CacheKey
struct CacheKeyHash {
  size_t operator()(const CacheKey &K) const {
    return hash_combine(K.F, K.MSSAProv, K.DTProv);
  }
};

/// Cache type: maps each cache key to its equivalence database
using CacheTy =
    std::unordered_map<CacheKey, std::unique_ptr<EquivDB>, CacheKeyHash>;
static CacheTy EquivCache;

/// Extract the parent function of a value
/// @param V The value (instruction or argument) to query
/// @return The function containing V, or nullptr if V is not function-scoped
///
/// This helper is used to determine which function's EquivDB should be
/// used for a query. Values must be within the same function to be compared.
const Function *getParentFunction(const Value *V) {
  // Instructions are contained in basic blocks, which are in functions
  if (auto *I = dyn_cast<Instruction>(V))
    return I->getParent()->getParent();
  // Arguments directly belong to functions
  if (auto *A = dyn_cast<Argument>(V))
    return A->getParent();
  // Global values, constants, etc. are not function-scoped
  return nullptr;
}

} // namespace

// ---------------------------------------------------------------------------
// Constructor and Destructor
// ---------------------------------------------------------------------------

UnderApproxAA::UnderApproxAA(Module &M) : _module(M) {
  // Note: The EquivDB instances are created lazily on first query
  // to avoid building analysis for functions that are never queried.
}

UnderApproxAA::~UnderApproxAA() {
  // The cache is static, so it persists across analysis instances
  // This is intentional: once a function's EquivDB is built, it can be
  // reused even if a new UnderApproxAA instance is created (assuming
  // the IR hasn't changed).
}

// ---------------------------------------------------------------------------
// AAResult interface implementation
// ---------------------------------------------------------------------------

AliasResult UnderApproxAA::alias(const MemoryLocation &L1,
                                 const MemoryLocation &L2) {
  // Extract pointer values from memory locations and delegate to mustAlias
  // Note: This ignores size information - we only check pointer equality,
  // not whether the memory regions overlap. This is acceptable for an
  // under-approximation: if pointers must alias, the locations must alias.
  return mustAlias(L1.Ptr, L2.Ptr) ? AliasResult::MustAlias
                                   : AliasResult::NoAlias;
}

bool UnderApproxAA::mustAlias(const Value *V1, const Value *V2) {
  // Early exit: ensure both values are valid pointers
  if (!isValidPointerQuery(V1, V2))
    return false;

  // Extract parent functions for both values
  const Function *F1 = getParentFunction(V1);
  const Function *F2 = getParentFunction(V2);

  // Cross-function queries: conservative under-approximation
  // We cannot prove must-alias across function boundaries without
  // inter-procedural analysis. Return false (NoAlias) conservatively.
  if (F1 != F2)
    return false;

  // Safety check: if no parent function found, cannot prove must-alias
  if (!F1)
    return false;

  // Safety check: ensure function has a parent module before building EquivDB
  if (!F1->getParent()) {
    return false;
  }

  // Get optional analyses from providers
  MemorySSA *MSSA = nullptr;
  DominatorTree *DT = nullptr;

  if (MSSAProvider) {
    MSSA = MSSAProvider(*const_cast<Function *>(F1));
  }
  if (DTProvider) {
    DT = DTProvider(*const_cast<Function *>(F1));
  }

  // Create cache key
  CacheKey Key{F1, MSSAProvider, DTProvider};

  // Lazy initialization: build EquivDB for F1 on first query
  // The cache ensures we only build it once per function+configuration
  auto &Ptr = EquivCache[Key];
  if (!Ptr) {
    // Note: const_cast is necessary because EquivDB constructor takes
    // a non-const Function&, but we only have const Function* from
    // getParentFunction. This is safe because EquivDB only reads the IR.
    try {
      Ptr = std::make_unique<EquivDB>(*const_cast<Function *>(F1), MSSA, DT);
    } catch (...) {
      // If EquivDB construction fails, return false (no must-alias)
      return false;
    }
  }

  // Safety check: ensure EquivDB was successfully created
  if (!Ptr)
    return false;

  // Query the equivalence database for this function
  return Ptr->mustAlias(V1, V2);
}

bool UnderApproxAA::isValidPointerQuery(const Value *v1,
                                        const Value *v2) const {
  // Validate that both values are non-null and have pointer types
  // This prevents invalid queries and avoids crashes
  return v1 && v2 && v1->getType()->isPointerTy() &&
         v2->getType()->isPointerTy();
}
