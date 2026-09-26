/**
 * \file LowerConstantExpr.h
 * \brief Pass for converting constant expressions to regular instructions
 * \author Lotus Team
 *
 * This pass converts constant expressions to regular instructions, which
 * can simplify analysis by eliminating complex constant expressions that
 * may be difficult to handle in downstream passes.
 */
#pragma once

#include <llvm/IR/Module.h>
#include <llvm/IR/PassManager.h>

using namespace llvm;

/**
 * \class LowerConstantExprPass
 * \brief Constant expression lowering pass
 *
 * This pass transforms constant expressions into regular instructions.
 * Constant expressions are expressions that can be computed at compile
 * time but exist as values in the IR. Converting them to instructions
 * can help:
 * - Simplify analysis passes
 * - Make the IR more uniform
 * - Enable further optimization
 */
class LowerConstantExprPass : public PassInfoMixin<LowerConstantExprPass> {
public:
  /**
   * \brief Run the constant expression lowering pass on a module
   * \param M The module to transform
   * \param MAM Module analysis manager
   * \return PreservedAnalyses indicating whether the module was modified
   */
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &MAM);
};

