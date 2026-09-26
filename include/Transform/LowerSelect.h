/**
 * \file LowerSelect.h
 * \brief Pass for lowering select instructions to conditional branches
 * \author Lotus Team
 *
 * This pass lowers select instructions to equivalent conditional branch
 * and phi structures, which can be useful for analysis tools that don't
 * handle select instructions directly.
 */
#pragma once

#include <llvm/IR/Module.h>
#include <llvm/IR/PassManager.h>

using namespace llvm;

/**
 * \class LowerSelectPass
 * \brief Select instruction lowering pass
 *
 * This pass converts select instructions into explicit control flow
 * using conditional branches and PHI nodes. This is useful for:
 * - Analysis tools that don't understand select instructions
 * - Making control flow more explicit
 * - Further optimization by other passes
 */
class LowerSelectPass : public PassInfoMixin<LowerSelectPass> {
public:
  /**
   * \brief Run the select lowering pass on a module
   * \param M The module to transform
   * \param MAM Module analysis manager
   * \return PreservedAnalyses indicating whether the module was modified
   */
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &MAM);
};

