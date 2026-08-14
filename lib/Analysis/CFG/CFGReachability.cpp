#include "Analysis/CFG/CFGReachability.h"

#include <llvm/IR/CFG.h>

using namespace llvm;

CFGReachability::CFGReachability(Function &F) : ReachableRows(F.size()) {
  unsigned Idx = 0;
  for (auto &B : F) {
    ID2BB.push_back(&B);
    BB2ID[&B] = Idx;
    ++Idx;
  }
}

// Returns true if there is a path from From to To in the CFG.
bool CFGReachability::reachable(BasicBlock *From, BasicBlock *To) {
  assert(From && To);
  assert(containsBlock(From) &&
         "CFGReachability: 'From' block not found — object may be stale");
  assert(containsBlock(To) &&
         "CFGReachability: 'To' block not found — object may be stale");

  if (From == To)
    return true;

  const unsigned DstBlockID = BB2ID.at(To);

  std::unique_lock<std::mutex> lock(CacheMutex);

  auto &Row = ReachableRows[DstBlockID];
  if (!Row) {
    Row.emplace(static_cast<unsigned>(ID2BB.size()), false);
    analyze(To); // analyze() must be called with the lock held
  }

  return (*Row)[BB2ID.at(From)];
}

// Returns true if there is a path from instruction From to instruction To.
//
// The old implementation only walked forward from From within the same
// block, returning false when To appeared earlier.  That is wrong for loops:
// if To precedes From in the block, From can still reach To via a back-edge
// that loops back to the block's header.
//
// Corrected logic for the same-block case:
//   1. Walk forward from From.  If we hit To before the end → reachable.
//   2. If we reach the end without finding To (To is before From) → reachable
//      iff the block can reach itself (i.e., it lies on a cycle).
bool CFGReachability::reachable(Instruction *From, Instruction *To) {
  assert(From && To);
  BasicBlock *FromB = From->getParent();
  BasicBlock *ToB = To->getParent();
  assert(FromB && ToB);
  assert(
      containsBlock(FromB) &&
      "CFGReachability: 'From' instruction is outside the analyzed function");
  assert(containsBlock(ToB) &&
         "CFGReachability: 'To' instruction is outside the analyzed function");

  if (From == To)
    return true;

  if (FromB == ToB) {
    if (From->comesBefore(To))
      return true;

    // To is before From in the block.  Reachable only if the block is on a
    // non-empty cycle. Block reachability itself is reflexive, so it cannot be
    // used to answer this question directly.
    return isOnCycle(FromB);
  }

  // Different blocks: delegate to block-level reachability.
  return reachable(FromB, ToB);
}

bool CFGReachability::isOnCycle(BasicBlock *BB) {
  assert(containsBlock(BB));
  for (BasicBlock *Succ : successors(BB)) {
    if (Succ == BB || reachable(Succ, BB))
      return true;
  }
  return false;
}

void CFGReachability::analyze(BasicBlock *ToBB) {
  const unsigned ToBBID = BB2ID[ToBB];
  BitVector VisitedVec(static_cast<unsigned>(ID2BB.size()));
  assert(ReachableRows[ToBBID]);
  ReachableVec &ToReachability = *ReachableRows[ToBBID];

  std::vector<BasicBlock *> Worklist;
  Worklist.push_back(ToBB);

  while (!Worklist.empty()) {
    BasicBlock *BB = Worklist.back();
    Worklist.pop_back();

    unsigned BBID = BB2ID[BB];
    if (VisitedVec[BBID])
      continue;
    VisitedVec[BBID] = true;

    // Mark BB as able to reach ToBB — but only if BB is not ToBB itself.
    // (Self-reachability for ToBB is handled by the From==To early-return in
    // reachable(), so we deliberately leave ToReachability[ToBBID] = false
    // here to avoid a spurious true when From != To but both map to ToBBID,
    // which cannot happen, but keeping the invariant explicit is cleaner.)
    if (BB != ToBB)
      ToReachability[BBID] = true;

    for (BasicBlock *Pred : predecessors(BB))
      Worklist.push_back(Pred);
  }
}
