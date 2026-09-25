/**
 * \file MergeReturn.h
 * \brief Pass for consolidating multiple return statements into a single return
 * \author Lotus Team
 *
 * This pass consolidates multiple return statements in a function into a
 * single unified return block, simplifying control flow analysis.
 */
#pragma once

#include <llvm/IR/Module.h>
#include <llvm/IR/PassManager.h>

using namespace llvm;

/**
 * \class MergeReturnPass
 * \brief Return consolidation pass for simplifying control flow
 *
 * This pass transforms functions with multiple return statements into functions
 * with a single return statement. This is useful for:
 * - Simplifying control flow analysis
 * - Making CFG traversals more straightforward
 * - Preparing code for further optimization
 *
 * The pass creates a new unified return block and redirects all existing
 * return statements to branch to this block. For non-void functions, PHI
 * nodes are created to select the appropriate return value.
 */
class MergeReturnPass : public PassInfoMixin<MergeReturnPass> {
public:
  /**
   * \brief Run the return merging pass on a module
   * \param M The module to transform
   * \param MAM Module analysis manager
   * \return PreservedAnalyses indicating whether the module was modified
   */
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &MAM);
};

