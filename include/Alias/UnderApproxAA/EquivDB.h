/**
 * @file EquivDB.h
 * @brief Equivalence database for must-alias analysis using union-find
 *
 * This file defines EquivDB, the core data structure for under-approximation
 * alias analysis. It implements union-find with congruence closure to track
 * equivalence classes of pointer values within a single function.
 */

#ifndef UNDERAPPROX_EQUIVDB_H
#define UNDERAPPROX_EQUIVDB_H

#include <vector>

#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/DenseSet.h>
#include <llvm/IR/Function.h>

// Forward declarations for optional analyses
namespace llvm {
class MemorySSA;
class DominatorTree;
class BranchInst;
class CmpInst;
} // namespace llvm

namespace UnderApprox {

/**
 * @class EquivDB
 * @brief Equivalence database: union-find with congruence closure over one
 * function's IR
 *
 * This class maintains equivalence classes of pointer values using union-find
 * data structures. Two pointers in the same equivalence class are guaranteed
 * to alias (must-alias). The database is built once during construction using
 * a two-phase algorithm (seed + propagate), then queried efficiently.
 *
 * Data Structures:
 * - Union-Find: Nodes array with parent pointers and ranks for efficient
 *   union/find operations with path compression and union-by-rank
 * - Value Mapping: Bidirectional mapping between LLVM Values and integer IDs
 * - Watch Lists: Per-class lists of instructions to revisit when classes merge
 * - Conditional Equivalences: Path-specific must-alias from branch conditions
 *
 * Algorithm:
 * 1. Seed phase: Apply atomic (syntactic) rules to find initial must-alias
 * pairs
 * 2. Propagate phase: Use semantic (inductive) rules to discover new
 * equivalences as classes merge, until saturation
 * 3. Path refinement: Use branch conditions to refine equivalences in dominated
 * blocks
 *
 * Time Complexity:
 * - Construction: O(N·M·α(N)) where N = values, M = instructions
 * - Query: O(α(N)) ≈ O(1) amortized (effectively constant)
 * - Memory: O(N) for union-find + watch lists
 */
class EquivDB {
public:
  /**
   * @brief Construct equivalence database for a function
   *
   * Builds the complete equivalence database by:
   * 1. Seeding with atomic must-alias pairs from syntactic rules
   * 2. Propagating equivalences using semantic rules until saturation
   * 3. Refining with path conditions (if DominatorTree available)
   *
   * After construction, queries are very fast (effectively constant time).
   *
   * @param F The LLVM function to analyze
   * @param MSSA Optional MemorySSA for store-load forwarding (sound)
   * @param DT Optional DominatorTree for path condition refinement (sound)
   */
  explicit EquivDB(llvm::Function &F, llvm::MemorySSA *MSSA = nullptr,
                   llvm::DominatorTree *DT = nullptr);

  /**
   * @brief Query if two values must alias
   *
   * Returns true if A and B are in the same equivalence class (must alias),
   * false if unknown (they may or may not alias, or weren't encountered
   * during construction).
   *
   * @param A First value to compare
   * @param B Second value to compare
   * @return true if A and B must alias, false otherwise (unknown)
   *
   * Time complexity: O(α(N)) ≈ O(1) amortized
   */
  bool mustAlias(const llvm::Value *A, const llvm::Value *B) const;

private:
  // ---------- Union-Find Data Structures ------------------------------------

  /// Integer ID type for union-find (each value gets a unique ID)
  using IdTy = unsigned;

  /// Union-find node: parent pointer and rank for union-by-rank optimization
  struct Node {
    IdTy Parent;  ///< Parent ID (self if root)
    uint8_t Rank; ///< Tree height estimate for union-by-rank
  };

  /// Get or create unique ID for a value
  /// @param V The value to get an ID for
  /// @return Unique integer ID for V
  IdTy id(const llvm::Value *);

  /// Find root of equivalence class (with path compression)
  /// @param X The ID to find root for
  /// @return Root ID of the class containing X
  IdTy find(IdTy);

  /// Unite two equivalence classes (union-by-rank with watch list merge)
  /// @param A First class ID
  /// @param B Second class ID
  void unite(IdTy, IdTy);

  /// Union-find forest: each index is a value ID, value is parent+rank
  std::vector<Node> Nodes;

  /// Reverse mapping: ID → Value (for debugging and watch list processing)
  std::vector<const llvm::Value *> Id2Val;

  /// Forward mapping: Value → ID (for fast lookups)
  llvm::DenseMap<const llvm::Value *, IdTy> Val2Id;

  // ---------- Watch Lists for Incremental Updates --------------------------

  /// Watch list entry: instructions that should be revisited when this class
  /// changes
  struct WatchInfo {
    /// Instructions watching this equivalence class
    /// When the class merges with another, these instructions are rechecked
    /// to see if semantic rules (e.g., closed PHI) can now fire
    llvm::SmallVector<llvm::Instruction *, 2> Users;
  };

  /// Watch lists indexed by union-find root ID
  /// Watches[i] contains instructions that depend on class i
  std::vector<WatchInfo> Watches;

  // ---------- Analysis Context ----------------------------------------------

  /// DataLayout for the target (needed for pointer size calculations)
  const llvm::DataLayout &DL;

  /// The function being analyzed
  llvm::Function &F;

  /// Optional MemorySSA for store-load forwarding
  llvm::MemorySSA *MSSA;

  /// Optional DominatorTree for path condition refinement
  llvm::DominatorTree *DT;

  // ---------- Construction Methods ------------------------------------------

  /// Phase 1: Seed worklist with atomic (syntactic) must-alias pairs
  /// Applies local pattern matching rules and registers watches
  /// @param WL Output worklist to populate
  void seedAtomicEqualities(
      std::vector<std::pair<const llvm::Value *, const llvm::Value *>> &WL);

  /// Phase 1a: Seed from direct calls that return a pointer argument
  /// Sound: callee returns arg_i ⇒ call result must-alias arg_i at call site
  /// @param WL Output worklist to populate
  void seedReturnValueForwarding(
      std::vector<std::pair<const llvm::Value *, const llvm::Value *>> &WL);

  /// Phase 1b (DT): Seed from single-store allocas when DominatorTree available
  /// Sound: one store to alloca dominating all loads ⇒ load equals stored value
  /// @param WL Output worklist to populate
  void seedSingleStoreAlloca(
      std::vector<std::pair<const llvm::Value *, const llvm::Value *>> &WL);

  /// Phase 2: Propagate equivalences using semantic (inductive) rules
  /// Processes worklist until saturation, revisiting watched instructions
  /// @param WL Worklist of must-alias pairs (modified in-place)
  void propagate(
      std::vector<std::pair<const llvm::Value *, const llvm::Value *>> &WL);

  /// Register an instruction to watch an operand's equivalence class
  /// When the class merges, the instruction will be revisited
  /// @param Op The operand whose class to watch
  /// @param I The instruction to register
  void registerWatch(const llvm::Value *Op, llvm::Instruction *I);

  /// Check if all pointer operands of an instruction are in the same class
  /// Used by semantic rules to detect closed patterns (PHI, Select, etc.)
  /// @param I The instruction to check
  /// @return true if all pointer operands unified, false otherwise
  bool operandsInSameClass(const llvm::Instruction *I) const;

  // ---------- Extension: Store-Load Forwarding with MemorySSA --------------

  /// Seed must-alias from MemorySSA store-load forwarding
  /// Sound: MemorySSA provides precise clobber information
  /// @param WL Output worklist to populate
  void seedStoreLoadForwarding(
      std::vector<std::pair<const llvm::Value *, const llvm::Value *>> &WL);

  // ---------- Extension: Path Condition Refinement -------------------------

  /// Refine equivalences using branch conditions
  /// Sound: if (p == q) then p ≡ q in the true branch
  /// @param WL Output worklist to populate with conditional equivalences
  void refineWithConditions(
      std::vector<std::pair<const llvm::Value *, const llvm::Value *>> &WL);

  /// Add must-alias pairs for conditionally executed blocks
  /// @param Cmp The comparison instruction
  /// @param TrueBB The block executed when condition is true
  /// @param FalseBB The block executed when condition is false
  /// @param WL Output worklist
  void addConditionalMustAlias(
      llvm::CmpInst *Cmp, llvm::BasicBlock *TrueBB, llvm::BasicBlock *FalseBB,
      std::vector<std::pair<const llvm::Value *, const llvm::Value *>> &WL);
};

} // end namespace UnderApprox
#endif