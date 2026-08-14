/** @file CFGReachability.h @brief CFG reachability analysis utilities. */
#ifndef ANALYSIS_CFG_CFGREACHABILITY_H
#define ANALYSIS_CFG_CFGREACHABILITY_H

#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

#include <llvm/ADT/BitVector.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instruction.h>

// Do NOT use `using namespace llvm` in headers — use explicit llvm:: qualifiers
// throughout to avoid polluting includers' namespaces.

/// CFGReachability - Provides reachability analysis for basic blocks and
/// instructions within a function's control flow graph.
///
/// Uses a lazy, demand-driven backward BFS: for each destination block queried,
/// a backward BFS from that block marks all predecessor blocks that can reach
/// it. Results are cached in a per-destination bit-vector.
///
/// Invalidation: any CFG mutation invalidates the object, including successor
/// rewrites that retain the same blocks. Discard and rebuild it after a
/// transform that does not preserve the CFG. containsBlock() only checks
/// membership in the block set captured at construction time; it cannot detect
/// edge mutations.
///
/// Memory: O(N + QN), where N is the number of blocks and Q is the number of
/// distinct destination blocks queried. Each row is allocated on first use.
///
/// Thread safety: reachable() is safe to call concurrently from multiple
/// threads. Internal state is protected by a mutex.
class CFGReachability {
private:
  using ReachableVec = llvm::BitVector;

  /// ReachableRows[dstID][srcID] == true iff src can reach dst. A missing row
  /// has not been queried yet.
  std::vector<std::optional<ReachableVec>> ReachableRows;

  /// ID mapping
  std::vector<llvm::BasicBlock *> ID2BB;
  std::map<llvm::BasicBlock *, unsigned> BB2ID;

  /// Protects lazy row creation and cached reachability results.
  mutable std::mutex CacheMutex;

public:
  explicit CFGReachability(llvm::Function &F);

  // Non-copyable: the mutex member is not copyable, and copying a large
  // reachability cache is almost never intentional.
  CFGReachability(const CFGReachability &) = delete;
  CFGReachability &operator=(const CFGReachability &) = delete;

  // Not movable: std::mutex is not movable, so the implicitly-deleted move
  // constructor/assignment cannot be defaulted.  Explicitly delete them to
  // suppress the -Wdefaulted-function-deleted warning and make the intent
  // clear.
  CFGReachability(CFGReachability &&) = delete;
  CFGReachability &operator=(CFGReachability &&) = delete;

  ~CFGReachability() = default;

  /// Returns true if \p BB was present when this object was constructed.
  /// This does not establish that the cached CFG is still current.
  bool containsBlock(llvm::BasicBlock *BB) const {
    return BB && BB2ID.count(BB) != 0;
  }

  /// Returns true if there is a path from \p From to \p To in the CFG.
  /// Both blocks must belong to the function passed at construction. Any CFG
  /// mutation after construction invalidates the object.
  bool reachable(llvm::BasicBlock *From, llvm::BasicBlock *To);

  /// Returns true if there is a path from instruction \p From to instruction
  /// \p To.
  ///
  /// Same-block case:
  ///   • If From comes before or at To in the block  → true (straight-line).
  ///   • If To comes before From in the block        → true iff the block can
  ///     reach itself through a non-empty path (i.e., it is in a cycle).
  bool reachable(llvm::Instruction *From, llvm::Instruction *To);

private:
  /// Backward BFS from \p ToBB: marks every block that has a path to ToBB.
  /// Must be called with CacheMutex held.
  void analyze(llvm::BasicBlock *ToBB);

  /// Returns true iff \p BB can reach itself through at least one CFG edge.
  /// Unlike reachable(BB, BB), this query is non-reflexive and therefore
  /// distinguishes a real cycle from the zero-length path.
  bool isOnCycle(llvm::BasicBlock *BB);
};

using CFGReachabilityRef = std::shared_ptr<CFGReachability>;

#endif // ANALYSIS_CFG_CFGREACHABILITY_H
