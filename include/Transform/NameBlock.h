/**
 * \file NameBlock.h
 * \brief Pass for naming basic blocks
 * \author Lotus Team
 *
 * This pass assigns meaningful names to basic blocks that don't have names,
 * improving code readability and debuggability.
 */
#pragma once

#include <llvm/IR/Module.h>
#include <llvm/IR/PassManager.h>

using namespace llvm;

/**
 * \class NameBlockPass
 * \brief Basic block naming pass
 *
 * This pass assigns descriptive names to unnamed basic blocks,
 * which helps with:
 * - Code readability
 * - Debugging
 * - Analysis and optimization logging
 */
class NameBlockPass : public PassInfoMixin<NameBlockPass> {
public:
  /**
   * \brief Run the naming pass on a module
   * \param M The module to transform
   * \param MAM Module analysis manager
   * \return PreservedAnalyses indicating whether the module was modified
   */
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &MAM);
};

