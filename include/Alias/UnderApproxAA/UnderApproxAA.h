/**
 * @file UnderApproxAA.h
 * @brief Under-approximation alias analysis using union-find with congruence
 * closure
 *
 * This file provides an under-approximation alias analysis that uses union-find
 * data structures with congruence closure to identify definite (must-alias)
 * relationships between pointers. The analysis is sound but incomplete: it only
 * reports MustAlias when it can guarantee the relationship, otherwise returns
 * NoAlias (conservative under-approximation).
 *
 * Key Features:
 * - Sound: Never produces false positives (if MustAlias, they definitely alias)
 * - Fast: O(α(N)) query time after construction (effectively constant)
 * - Intra-procedural: Analyzes within a single function at a time
 * - Per-function caching: Equivalence databases are built once per function
 * - Optional MemorySSA: Store-load forwarding when available (sound)
 * - Optional DominatorTree: Single-store alloca forwarding when available
 *
 * Algorithm Overview:
 * The analysis uses a three-phase approach:
 * 1. Seed: Apply atomic (syntactic) rules to find initial must-alias pairs
 * 2. Propagate: Use semantic (inductive) rules to discover additional
 * equivalences
 * 3. Refine: Apply external analyses (MemorySSA, DominatorTree) for more
 * precision
 *
 * This analysis is useful when:
 * - A lightweight, fast alias analysis is needed
 * - Only definite aliases are required (precision over recall)
 * - Soundness is critical (no false positives allowed)
 * - More sophisticated inter-procedural analyses are unavailable or too
 * expensive
 *
 * See README.md for detailed documentation on rules, examples, and limitations.
 */

#pragma once

#include <llvm/Analysis/AliasAnalysis.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Value.h>

// Forward declarations for optional analyses
namespace llvm {
class MemorySSA;
class DominatorTree;
class Function;
} // namespace llvm

namespace UnderApprox {

/**
 * @class UnderApproxAA
 * @brief Under-approximation alias analysis implementation
 *
 * This class implements a conservative alias analysis that uses union-find with
 * congruence closure to identify definite (MustAlias) relationships. It
 * recognizes patterns such as:
 * - Identity: same SSA value
 * - Cast equivalence: bitcasts, no-op address space casts
 * - GEP patterns: zero-offset GEPs, constant-offset equivalence
 * - Round-trip casts: inttoptr(ptrtoint(X)) ≡ X (and enhanced with no-op
 * arithmetic)
 * - Trivial PHI/Select: all operands identical
 * - Same underlying object: derived from same alloca/global/allocation
 *
 * With optional MemorySSA:
 * - Store-load forwarding: load after store to same location = stored value
 *
 * With optional DominatorTree:
 * - Single-store alloca forwarding: load-after-dominating-store precision
 *
 * The analysis is an under-approximation: it only reports MustAlias when
 * certain, otherwise returns NoAlias. It never reports MayAlias, making it
 * suitable for optimizations that require definite knowledge (e.g., redundant
 * load elimination).
 *
 * Performance:
 * - Construction: O(N·M·α(N)) where N = number of values, M = instructions
 * - Query: O(α(N)) ≈ O(1) amortized (effectively constant)
 * - Memory: O(N) for union-find structures and watch lists
 */
class UnderApproxAA {
public:
  /**
   * @brief Construct an under-approximation alias analysis
   *
   * Creates an analysis instance for the given module. The analysis uses lazy
   * initialization: equivalence databases are built per-function on first
   * query.
   *
   * @param M The LLVM module to analyze
   */
  UnderApproxAA(llvm::Module &M);

  /**
   * @brief Destructor
   *
   * Note: The per-function cache is static and persists across instances.
   * This allows reuse of built databases even if new UnderApproxAA instances
   * are created (assuming the IR hasn't changed).
   */
  ~UnderApproxAA();

  /**
   * @brief Query alias relationship between two values
   *
   * This is a convenience wrapper around mustAlias() that returns an
   * AliasResult. Note: This method is deprecated in favor of alias() with
   * MemoryLocation.
   *
   * @param v1 First value (must be pointer type)
   * @param v2 Second value (must be pointer type)
   * @return AliasResult indicating the alias relationship
   *         (either MustAlias or NoAlias, never MayAlias)
   *
   * @deprecated Use alias(MemoryLocation, MemoryLocation) instead
   */
  llvm::AliasResult query(const llvm::Value *v1, const llvm::Value *v2);

  /**
   * @brief Query alias relationship between two memory locations
   *
   * This is the standard LLVM AAResult interface method. It extracts pointer
   * values from memory locations and delegates to mustAlias().
   *
   * Note: Size information in MemoryLocation is ignored - we only check pointer
   * equality. This is acceptable for an under-approximation: if pointers must
   * alias, the memory locations must alias regardless of size.
   *
   * @param loc1 First memory location
   * @param loc2 Second memory location
   * @return AliasResult::MustAlias if pointers must alias, NoAlias otherwise
   */
  llvm::AliasResult alias(const llvm::MemoryLocation &loc1,
                          const llvm::MemoryLocation &loc2);

  /**
   * @brief Check if two values must alias
   *
   * This is the core query method. It checks if two pointer values are in the
   * same equivalence class, indicating they must alias.
   *
   * Behavior:
   * - Returns true only if both values are in the same function and guaranteed
   *   to alias (same equivalence class)
   * - Returns false if values are in different functions (cross-function
   * queries are not supported)
   * - Returns false if values are not valid pointers
   * - Returns false if alias relationship is unknown (conservative)
   *
   * @param v1 First value (should be pointer type)
   * @param v2 Second value (should be pointer type)
   * @return true if v1 and v2 must alias, false otherwise (unknown or different
   * functions)
   *
   * Time complexity: O(α(N)) ≈ O(1) amortized after initial construction
   */
  bool mustAlias(const llvm::Value *v1, const llvm::Value *v2);

  /**
   * @brief Set MemorySSA provider for enhanced analysis
   *
   * When set, store-load forwarding will be enabled using MemorySSA.
   * This provides more precise must-alias detection.
   *
   * @param Provider A function that returns MemorySSA for a given function
   */
  using MemorySSAProvider = llvm::MemorySSA *(*)(llvm::Function & F);
  void setMemorySSAProvider(MemorySSAProvider Provider) {
    MSSAProvider = Provider;
  }

  /**
   * @brief Set DominatorTree provider for enhanced analysis
   *
   * When set, DominatorTree-based single-store alloca forwarding is enabled.
   * This provides additional precise must-alias facts for load/store patterns.
   *
   * @param Provider A function that returns DominatorTree for a given function
   */
  using DominatorTreeProvider = llvm::DominatorTree *(*)(llvm::Function & F);
  void setDominatorTreeProvider(DominatorTreeProvider Provider) {
    DTProvider = Provider;
  }

  /**
   * @brief Get the module being analyzed
   * @return Reference to the module
   */
  llvm::Module &getModule() { return _module; }

private:
  /// The module being analyzed
  llvm::Module &_module;

  /// Optional MemorySSA provider for store-load forwarding
  MemorySSAProvider MSSAProvider = nullptr;

  /// Optional DominatorTree provider for single-store alloca forwarding
  DominatorTreeProvider DTProvider = nullptr;

  /**
   * @brief Validate that two values are valid for pointer alias queries
   *
   * Checks that both values are non-null and have pointer types. This prevents
   * invalid queries and avoids crashes during analysis.
   *
   * @param v1 First value to validate
   * @param v2 Second value to validate
   * @return true if both values are valid pointers, false otherwise
   */
  bool isValidPointerQuery(const llvm::Value *v1, const llvm::Value *v2) const;
};

} // namespace UnderApprox
