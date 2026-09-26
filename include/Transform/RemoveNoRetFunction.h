/**
 * \file RemoveNoRetFunction.h
 * \brief Pass for removing functions that don't return
 * \author Lotus Team
 *
 * This pass identifies and removes function calls to functions that are
 * marked as noreturn, optimizing control flow and eliminating dead code.
 */
#pragma once

#include <llvm/IR/Module.h>
#include <llvm/IR/PassManager.h>

using namespace llvm;

/**
 * \class RemoveNoRetFunctionPass
 * \brief Noreturn function removal pass
 *
 * This pass identifies calls to noreturn functions and removes them,
 * along with any unreachable code that follows. This helps:
 * - Clean up dead code after noreturn calls
 * - Simplify control flow
 * - Improve analysis accuracy
 */
class RemoveNoRetFunctionPass : public PassInfoMixin<RemoveNoRetFunctionPass> {
public:
  /**
   * \brief Run the noreturn function removal pass on a module
   * \param M The module to transform
   * \param MAM Module analysis manager
   * \return PreservedAnalyses indicating whether the module was modified
   */
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &MAM);
};

