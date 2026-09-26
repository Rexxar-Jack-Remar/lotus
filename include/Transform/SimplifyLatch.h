/**
 * \file SimplifyLatch.h
 * \brief Pass for simplifying loop latch blocks
 * \author Lotus Team
 *
 * This pass performs simplifications on loop latch blocks to optimize
 * loop structure and control flow.
 */
#pragma once

#include <llvm/IR/Module.h>
#include <llvm/IR/PassManager.h>

using namespace llvm;

/**
 * \class SimplifyLatchPass
 * \brief Loop latch simplification pass
 *
 * This pass analyzes and simplifies loop latch blocks to improve
 * loop structure and enable further optimizations. It handles
 * common patterns in loop latch code that can be optimized.
 */
class SimplifyLatchPass : public PassInfoMixin<SimplifyLatchPass> {
public:
  /**
   * \brief Run the latch simplification pass on a module
   * \param M The module to transform
   * \param MAM Module analysis manager
   * \return PreservedAnalyses indicating whether the module was modified
   */
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &MAM);
};

