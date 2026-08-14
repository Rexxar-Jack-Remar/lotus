#include "Analysis/CFG/CodeMetrics.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/Analysis/CFG.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <vector>

using namespace llvm;

// ---------------------------------------------------------------------------
// 1. Cyclomatic Complexity
// ---------------------------------------------------------------------------

unsigned calcCyclomaticComplexity(Function &F) {
  if (F.isDeclaration() || F.empty())
    return 0;

  uint64_t Complexity = 1;

  // Count the surplus outgoing edges at each decision point. This is
  // equivalent to E - N + NumExits + 1 for an entry-reachable CFG, but also
  // handles multiple exits and CFGs with no exit without special cases.
  ReversePostOrderTraversal<Function *> RPOT(&F);
  for (BasicBlock *BB : RPOT) {
    const uint64_t NumSuccessors = succ_size(BB);
    if (NumSuccessors > 1)
      Complexity += NumSuccessors - 1;
  }

  return static_cast<unsigned>(Complexity);
}

// ---------------------------------------------------------------------------
// 2. Loop count / max nesting depth
// ---------------------------------------------------------------------------

static void scanLoop(const Loop *L, unsigned Depth, LoopMetrics &M) {
  ++M.NumLoops;
  M.MaxDepth = std::max(M.MaxDepth, Depth);
  for (auto *Child : L->getSubLoops())
    scanLoop(Child, Depth + 1, M);
}

LoopMetrics collectLoopMetrics(Function &F, LoopInfo &LI) {
  (void)F;
  LoopMetrics M;
  for (auto *Top : LI)
    scanLoop(Top, 1, M);
  return M;
}

// ---------------------------------------------------------------------------
// 3. NPath complexity
//
// Uses an iterative topological-order traversal with back-edge skipping to
// avoid infinite recursion on loops (old recursive version had no cycle
// detection). Uses saturating uint64_t addition to avoid silent overflow.
// ---------------------------------------------------------------------------

/// Saturating addition for uint64_t — returns UINT64_MAX on overflow.
static inline uint64_t sat_add(uint64_t a, uint64_t b) {
  if (b > std::numeric_limits<uint64_t>::max() - a)
    return std::numeric_limits<uint64_t>::max();
  return a + b;
}

uint64_t nPath(Function &F) {
  if (F.empty())
    return 0;

  // Collect back-edges so we can skip them during traversal.
  SmallVector<std::pair<const BasicBlock *, const BasicBlock *>, 8> backEdges;
  FindFunctionBackedges(F, backEdges);
  std::sort(backEdges.begin(), backEdges.end());

  auto isBackEdge = [&](const BasicBlock *Src, const BasicBlock *Dst) -> bool {
    return std::binary_search(backEdges.begin(), backEdges.end(),
                              std::make_pair(Src, Dst));
  };

  // paths[BB] = number of acyclic paths from BB to any exit, ignoring
  // back-edges.  Computed in reverse RPO (exits first).
  DenseMap<const BasicBlock *, uint64_t> paths;

  ReversePostOrderTraversal<const Function *> RPOT(&F);
  std::vector<const BasicBlock *> order(RPOT.begin(), RPOT.end());

  for (auto it = order.rbegin(); it != order.rend(); ++it) {
    const BasicBlock *BB = *it;
    uint64_t sum = 0;
    for (const BasicBlock *Succ : successors(BB)) {
      if (isBackEdge(BB, Succ))
        continue;
      auto it2 = paths.find(Succ);
      uint64_t succPaths = (it2 != paths.end()) ? it2->second : 0;
      sum = sat_add(sum, succPaths);
    }
    // Only original CFG terminals complete a path. A block whose successors
    // are all suppressed back-edges is not a synthetic exit.
    paths[BB] = succ_empty(BB) ? 1 : sum;
  }

  return paths[&F.getEntryBlock()];
}

// ---------------------------------------------------------------------------
// Legacy pass manager wrapper
// ---------------------------------------------------------------------------

ComplexityLegacy::ComplexityLegacy() : FunctionPass(ID) {}

bool ComplexityLegacy::runOnFunction(Function &F) {
  auto &LI = getAnalysis<LoopInfoWrapperPass>().getLoopInfo();
  auto CC = calcCyclomaticComplexity(F);
  auto LM = collectLoopMetrics(F, LI);
  auto NP = nPath(F);

  errs() << "== " << F.getName() << " ==\n"
         << "  Cyclomatic    : " << CC << '\n'
         << "  NPath         : ";
  if (NP == std::numeric_limits<uint64_t>::max())
    errs() << ">= UINT64_MAX (saturated)\n";
  else
    errs() << NP << '\n';
  errs() << "  Loops         : " << LM.NumLoops << "  (max depth "
         << LM.MaxDepth << ")\n";

  return false; // analysis pass — does not modify IR
}

void ComplexityLegacy::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.setPreservesAll();
  AU.addRequired<LoopInfoWrapperPass>();
  AU.addPreserved<LoopInfoWrapperPass>();
}

char ComplexityLegacy::ID = 0;

static RegisterPass<ComplexityLegacy> X("complexity-legacy",
                                        "Complexity metrics (legacy PM)",
                                        /*cfgOnly=*/false,
                                        /*is_analysis=*/true);
