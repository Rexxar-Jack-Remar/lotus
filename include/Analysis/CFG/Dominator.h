/*
 * Copyright 2016 - 2022  Angelo Matni, Simone Campanoni
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 of this software and associated documentation files (the "Software"), to deal
 in the Software without restriction, including without limitation the rights to
 use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
 of the Software, and to permit persons to whom the Software is furnished to do
 so, subject to the following conditions:

 * The above copyright notice and this permission notice shall be included in
 all copies or substantial portions of the Software.

 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
 DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE
 OR OTHER DEALINGS IN THE SOFTWARE.
 */
/**
 * @file Dominator.h
 * @brief Dominator and Post-Dominator Analysis
 *
 * This file provides classes for computing dominator and post-dominator trees
 * of LLVM IR functions. Dominators are fundamental data-flow analysis results
 * used in compiler optimizations, static analysis, and program verification.
 *
 * Dominators:
 * - A node A dominates node B if every path from the entry to B must go through
 * A
 * - The immediate dominator of B is the unique closest dominator of B
 * (excluding B)
 * - The dominator tree organizes these relationships hierarchically
 *
 * Post-Dominators:
 * - A node A post-dominates node B if every path from B to the exit must go
 * through A
 * - Computed similarly to dominators but on the reversed CFG
 *
 * @author Lotus Analysis Framework
 * @date 2025
 * @ingroup CFG
 */

/** @file Dominator.h @brief Dominator analysis for control-flow graphs. */
#ifndef NOELLE_SRC_CORE_DOMINATORS_H_
#define NOELLE_SRC_CORE_DOMINATORS_H_

#include "Analysis/CFG/DominatorForest.h"
#include "Analysis/CFG/DominatorNode.h"

namespace noelle {

/**
 * @class DominatorSummary
 * @brief Provides dominator and post-dominator information for a function
 *
 * This class encapsulates both the dominator tree (DT) and post-dominator tree
 * (PDT) for a function, providing a unified interface for dominance-related
 * queries.
 *
 * Dominator trees enable efficient queries about:
 * - Whether one instruction/basic block dominates another
 * - The nearest common dominator of two nodes
 * - The dominance frontier
 * - Variable liveness analysis
 *
 * @note DominatorSummary is typically constructed by Dominator or PostDominator
 * passes
 * @see DominatorForest for the underlying tree data structure
 */
class DominatorSummary {
public:
  /**
   * @brief Construct a dominator summary from full dominator and post-dominator
   * trees
   * @param DT The dominator tree to use
   * @param PDT The post-dominator tree to use
   */
  DominatorSummary(DominatorTree &DT, PostDominatorTree &PDT);

  /**
   * @brief Construct a dominator summary for a subset of basic blocks
   * @param DS The original dominator summary to subset from
   * @param bbSubset The set of basic blocks to include in the subset
   */
  DominatorSummary(DominatorSummary &DS, std::set<BasicBlock *> &bbSubset);

  DominatorForest DT;  ///< Dominator tree for the function
  DominatorForest PDT; ///< Post-dominator tree for the function
};

} // namespace noelle

#endif // NOELLE_SRC_CORE_DOMINATORS_H_