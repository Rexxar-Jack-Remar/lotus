#ifndef ANALYSIS_CFG_CFGREACHABILITY_H
#define ANALYSIS_CFG_CFGREACHABILITY_H

#include <llvm/ADT/BitVector.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instruction.h>

#include <map>
#include <memory>
#include <vector>

// Fix #7 (L7): Do NOT use `using namespace llvm` in headers — use explicit
// llvm:: qualifiers throughout to avoid polluting includers' namespaces.

/// CFGReachability - Provides reachability analysis for basic blocks and
/// instructions within a function's control flow graph.
///
/// Uses a lazy, demand-driven backward BFS: for each destination block queried,
/// a backward BFS from that block marks all predecessor blocks that can reach
/// it. Results are cached in a per-destination bit-vector.
///
/// Memory: O(N^2) in the number of basic blocks (one bit-vector row per
/// destination that is actually queried).
///
/// Thread safety: not thread-safe; external synchronization required for
/// concurrent queries.
class CFGReachability {
private:
  using ReachableVec = llvm::BitVector;

  /// One bit per block: has analyze() been run for this destination?
  ReachableVec AnalyzedVec;

  /// ReachableMatrix[dstID][srcID] == true  iff  src can reach dst.
  /// Stored as a vector of bit-vectors to avoid raw new[]/delete[].
  // Fix #2: replaced raw `ReachableVec *ReachableVecPtr` (Rule-of-Three
  // violation) with std::vector so the class is safely copyable/movable
  // without manual memory management.
  std::vector<ReachableVec> ReachableMatrix;

  /// ID mapping
  std::vector<llvm::BasicBlock *> ID2BB;
  std::map<llvm::BasicBlock *, unsigned> BB2ID;

public:
  explicit CFGReachability(llvm::Function *F);

  // Compiler-generated copy/move/destructor are all correct now that
  // ReachableMatrix is a std::vector.  Explicitly default them for clarity.
  CFGReachability(const CFGReachability &) = default;
  CFGReachability(CFGReachability &&) = default;
  CFGReachability &operator=(const CFGReachability &) = default;
  CFGReachability &operator=(CFGReachability &&) = default;
  ~CFGReachability() = default;

  /// Returns true if there is a path from \p From to \p To in the CFG.
  /// Both blocks must belong to the function passed at construction.
  bool reachable(llvm::BasicBlock *From, llvm::BasicBlock *To);

  /// Returns true if there is a path from instruction \p From to instruction
  /// \p To.
  ///
  /// Fix #3: When both instructions are in the same basic block the old code
  /// only walked forward from From, returning false if To appeared earlier —
  /// which is wrong for loops (To could be reached via a back-edge).  The
  /// corrected logic:
  ///   • If From comes before or at To in the block  → true (straight-line).
  ///   • If To comes before From in the block        → true iff the block can
  ///     reach itself (i.e., it is in a cycle).
  bool reachable(llvm::Instruction *From, llvm::Instruction *To);

private:
  /// Backward BFS from \p ToBB: marks every block that has a path to ToBB.
  /// Fix #1: replaced the fragile FirstRun boolean with an explicit
  /// `BB != ToBB` guard so the intent is immediately clear.
  void analyze(llvm::BasicBlock *ToBB);
};

using CFGReachabilityRef = std::shared_ptr<CFGReachability>;

#endif // ANALYSIS_CFG_CFGREACHABILITY_H
