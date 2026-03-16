/**
 * @file EquivDB.h
 * @brief Equivalence database for lightweight must-alias analysis
 *
 * This file defines EquivDB, the core data structure for under-approximation
 * alias analysis. It implements union-find, normalized pointer-term
 * congruence, and a singleton-slot memory abstraction to track equivalence
 * classes of pointer values within a single function.
 */

#ifndef UNDERAPPROX_EQUIVDB_H
#define UNDERAPPROX_EQUIVDB_H

#include <cstdint>
#include <unordered_map>
#include <vector>

#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/DenseSet.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Operator.h>

// Forward declarations for optional analyses
namespace llvm {
class MemorySSA;
class DominatorTree;
} // namespace llvm

namespace UnderApprox {

/**
 * @class EquivDB
 * @brief Equivalence database: union-find with normalized term congruence over
 * function's IR
 *
 * This class maintains equivalence classes of pointer values using union-find
 * data structures, congruence over normalized pointer terms, and singleton
 * memory-slot reasoning. Two pointers in the same equivalence class are
 * guaranteed to alias (must-alias). The database is built once during
 * construction using a two-phase algorithm (seed + propagate), then queried
 * efficiently.
 *
 * Data Structures:
 * - Union-Find: Nodes array with parent pointers and ranks for efficient
 *   union/find operations with path compression and union-by-rank
 * - Value Mapping: Bidirectional mapping between LLVM Values and integer IDs
 * - Watch Lists: Per-class lists of instructions to revisit when classes merge
 * - Term Buckets: Congruence table for normalized pointer terms
 * - Singleton Slot IDs: Memory abstraction for alloca/global/direct-allocation
 *   constant offsets
 * - Worklist deduplication: Avoids redundant pair processing
 *
 * Algorithm:
 * 1. Seed phase: Apply atomic (syntactic) rules to find initial must-alias
 * pairs
 * 2. Propagate phase: Rebuild normalized pointer terms and discover new
 * equivalences as classes merge, until saturation
 * 3. Optional seeding extensions (MemorySSA, DominatorTree, call summaries)
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
   * 2. Propagating equivalences using normalized term congruence until
   *    saturation
   * 3. Optional DominatorTree/MemorySSA seeding for singleton memory slots
   *
   * After construction, queries are very fast (effectively constant time).
   *
   * @param F The LLVM function to analyze
   * @param MSSA Optional MemorySSA for store-load forwarding (sound)
   * @param DT Optional DominatorTree for single-store alloca forwarding
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

  /// Optional DominatorTree for single-store alloca forwarding
  llvm::DominatorTree *DT;

  /// Pair keys already queued/processed during this build
  llvm::DenseSet<uint64_t> SeenPairKeys;

  /// GEP candidates bucketed by index-signature hash
  llvm::DenseMap<uint64_t,
                 llvm::SmallVector<const llvm::GetElementPtrInst *, 4>>
      GEPIndexBuckets;

  enum class TermKind : uint8_t { Invalid, GEPConst, GEPIndex };

  struct SlotId {
    const llvm::Value *Object = nullptr;
    int64_t ByteOffset = 0;

    bool operator==(const SlotId &Other) const {
      return Object == Other.Object && ByteOffset == Other.ByteOffset;
    }
  };

  struct SlotIdHash {
    size_t operator()(const SlotId &Slot) const;
  };

  struct TermKey {
    TermKind Kind = TermKind::Invalid;
    std::vector<uint64_t> Components;

    bool operator==(const TermKey &Other) const {
      return Kind == Other.Kind && Components == Other.Components;
    }
  };

  struct TermKeyHash {
    size_t operator()(const TermKey &Key) const;
  };

  /// Congruence table for normalized pointer terms.
  std::unordered_map<TermKey, llvm::SmallVector<const llvm::Value *, 4>,
                     TermKeyHash>
      TermBuckets;

  /// Stable numbering for singleton memory slots used by load/store reasoning.
  std::unordered_map<SlotId, uint64_t, SlotIdHash> SlotNumbers;

  struct ReturnSummary {
    enum class Kind : uint8_t { Unknown, Arg, Null, Global };
    Kind K = Kind::Unknown;
    int ArgNo = -1;
    const llvm::Value *Fixed = nullptr;
  };

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

  /// Phase 1b (DT): Seed from singleton slots when DominatorTree available.
  /// Sound: one store to a singleton slot dominating all loads from that slot
  /// ⇒ load equals stored value.
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

  // ---------- Extension: Store-Load Forwarding with MemorySSA --------------

  /// Seed must-alias from MemorySSA store-load forwarding
  /// Sound: MemorySSA provides precise clobber information
  /// @param WL Output worklist to populate
  void seedStoreLoadForwarding(
      std::vector<std::pair<const llvm::Value *, const llvm::Value *>> &WL);

  // ---------- Helpers -------------------------------------------------------

  void buildGEPIndex();
  bool findClosedGEPCandidate(const llvm::GetElementPtrInst *GEPI,
                              const llvm::Value *&Rep) const;
  bool equivalentGEPIndexOperand(const llvm::Value *A,
                                 const llvm::Value *B) const;
  uint64_t gepIndexSignature(const llvm::Value *GEP) const;
  const llvm::Value *valueForRoot(IdTy Root) const;
  const llvm::Value *resolveNormalizedValue(const llvm::Value *V) const;
  bool buildNormalizedTermKey(const llvm::Value *V, TermKey &Key) const;
  bool termsCongruent(const llvm::Value *A, const llvm::Value *B,
                      const TermKey &Key) const;
  void propagateNormalizedTerm(
      llvm::Instruction *I,
      std::vector<std::pair<const llvm::Value *, const llvm::Value *>> &WL);
  bool tryGetSingletonSlot(const llvm::Value *Ptr, SlotId &Out) const;
  bool sameSingletonSlot(const llvm::Value *P, const llvm::Value *Q) const;
  uint64_t internSlot(const SlotId &Slot);

  bool dominatesInst(const llvm::Instruction *Def,
                     const llvm::Instruction *Use) const;

  uint64_t makePairKey(const llvm::Value *A, const llvm::Value *B);
  bool enqueuePair(
      std::vector<std::pair<const llvm::Value *, const llvm::Value *>> &WL,
      const llvm::Value *A, const llvm::Value *B);

  ReturnSummary summarizeReturnBehavior(const llvm::Function *Callee) const;
  int resolveReturnedArgNo(const llvm::Value *V, const llvm::Function *Callee,
                           unsigned Depth = 0) const;
};

} // end namespace UnderApprox
#endif
