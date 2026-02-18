// Fix #9: Added include guard. The original file had none, causing ODR
// violations and duplicate RegisterPass<> calls when included more than once.
#ifndef ANALYSIS_CFG_CODEMETRICS_H
#define ANALYSIS_CFG_CODEMETRICS_H

/*======================================================================*\
|  "ComplexityMetrics" – one-stop shop for quick-and-dirty metrics       |
|                                                                        |
| - Cyclomatic complexity: Measures the number of independent paths      |
|   through code by counting decision points (if, while, for, case).    |
|   Higher values mean more complex code that's harder to test.          |
|                                                                        |
| - Loop count / max nesting depth: Loop count tracks how many loops     |
|   exist in code. Max nesting depth measures how deeply nested your     |
|   control structures are. Deep nesting makes code hard to maintain.    |
|                                                                        |
| - NPath complexity: Counts the total number of unique execution paths  |
|   through a function, considering all possible combinations of         |
|   branches and loops. Grows exponentially with nested conditions.      |
\*======================================================================*/

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/Analysis/CallGraph.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/DebugInfo.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/Pass.h"
#include "llvm/Support/raw_ostream.h"

#include <cmath>
#include <limits>

using namespace llvm;

/*-------------------------------------------------------------*
 * 1. Cyclomatic Complexity                                    *
 *-------------------------------------------------------------*/
static unsigned calcCyclomaticComplexity(Function &F) {
  unsigned Blocks = 0, Edges = 0, Calls = 0;

  for (auto &BB : F) {
    ++Blocks;
    for (auto *Succ : successors(&BB)) {
      (void)Succ;
      ++Edges;
    }
    for (auto &I : BB)
      if (isa<CallInst>(I) || isa<InvokeInst>(I))
        ++Calls;
  }

  /*  V(G) = E – N + 2P,  P==1 for a single function  */
  return 2 + Calls + Edges - Blocks;
}

/*-------------------------------------------------------------*
 * 2. Loop count / max nesting depth                           *
 *-------------------------------------------------------------*/
struct LoopMetrics { unsigned NumLoops = 0, MaxDepth = 0; };

static void scanLoop(const Loop *L, unsigned Depth, LoopMetrics &M) {
  ++M.NumLoops;
  M.MaxDepth = std::max(M.MaxDepth, Depth);
  for (auto *Child : L->getSubLoops())
    scanLoop(Child, Depth + 1, M);
}

static LoopMetrics collectLoopMetrics(Function &F, LoopInfo &LI) {
  LoopMetrics M;
  for (auto *Top : LI)
    scanLoop(Top, 1, M);
  return M;
}

/*-------------------------------------------------------------*
 * 3. NPath complexity                                         *
 *                                                             *
 * Fix #7: The original recursive implementation had no cycle  *
 * detection. For any CFG with a loop the recursion would      *
 * follow back-edges indefinitely, causing a stack overflow.   *
 *                                                             *
 * Fix: use an iterative topological-order traversal.          *
 * Back-edges (identified by FindFunctionBackedges) are        *
 * skipped so that each block is processed exactly once.       *
 *                                                             *
 * Fix #8: NPath grows exponentially and silently overflows    *
 * uint64_t. Added saturating addition: once the value reaches *
 * UINT64_MAX it stays there and the caller can treat it as    *
 * "effectively infinite".                                     *
 *-------------------------------------------------------------*/

/// Saturating addition for uint64_t — returns UINT64_MAX on overflow.
static inline uint64_t sat_add(uint64_t a, uint64_t b) {
  // Fix #8: detect overflow before it happens.
  if (b > std::numeric_limits<uint64_t>::max() - a)
    return std::numeric_limits<uint64_t>::max();
  return a + b;
}

static uint64_t nPath(Function &F) {
  if (F.empty())
    return 0;

  // Fix #7: collect back-edges so we can skip them during traversal.
  llvm::SmallVector<std::pair<const BasicBlock *, const BasicBlock *>, 8>
      backEdges;
  FindFunctionBackedges(F, backEdges);
  // Sort for O(log N) lookup via binary_search.
  std::sort(backEdges.begin(), backEdges.end());

  auto isBackEdge = [&](const BasicBlock *Src,
                        const BasicBlock *Dst) -> bool {
    return std::binary_search(backEdges.begin(), backEdges.end(),
                              std::make_pair(Src, Dst));
  };

  // paths[BB] = number of acyclic paths from BB to any exit, ignoring
  // back-edges.  We compute this in reverse post-order (topological order
  // w.r.t. the DAG obtained by removing back-edges).
  DenseMap<const BasicBlock *, uint64_t> paths;

  // Reverse post-order gives us a topological order for the DAG.
  llvm::ReversePostOrderTraversal<const Function *> RPOT(&F);
  // Collect in reverse so we process successors before predecessors.
  std::vector<const BasicBlock *> order(RPOT.begin(), RPOT.end());
  // Process in reverse RPO (i.e., exits first).
  for (auto it = order.rbegin(); it != order.rend(); ++it) {
    const BasicBlock *BB = *it;
    uint64_t sum = 0;
    bool hasNonBackSucc = false;
    for (const BasicBlock *Succ : successors(BB)) {
      if (isBackEdge(BB, Succ))
        continue; // Fix #7: skip back-edges to break cycles.
      hasNonBackSucc = true;
      uint64_t succPaths = 0;
      auto it2 = paths.find(Succ);
      if (it2 != paths.end())
        succPaths = it2->second;
      sum = sat_add(sum, succPaths); // Fix #8: saturating add.
    }
    // A block with no non-back-edge successors is an exit (or a loop
    // latch whose only successor is a back-edge target): count as 1 path.
    paths[BB] = hasNonBackSucc ? sum : 1;
  }

  return paths[&F.getEntryBlock()];
}

/*-------------------------------------------------------------*
 * Legacy PM glue                                              *
 *-------------------------------------------------------------*/
namespace {
struct ComplexityLegacy : public FunctionPass {
  static char ID;
  ComplexityLegacy() : FunctionPass(ID) {}

  bool runOnFunction(Function &F) override {
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
    errs() << "  Loops         : " << LM.NumLoops
           << "  (max depth " << LM.MaxDepth << ")\n";

    return false; /* analysis pass */
  }

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.setPreservesAll();
    AU.addRequired<LoopInfoWrapperPass>();
    AU.addPreserved<LoopInfoWrapperPass>();
  }
};
} // end anonymous namespace

char ComplexityLegacy::ID = 0;
static RegisterPass<ComplexityLegacy>
    X("complexity-legacy", "Complexity metrics (legacy PM)",
      /*cfgOnly=*/false, /*is_analysis=*/true);

#endif // ANALYSIS_CFG_CODEMETRICS_H
