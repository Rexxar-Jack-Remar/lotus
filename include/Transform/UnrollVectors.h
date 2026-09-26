/**
 * \file UnrollVectors.h
 * \brief Pass for unrolling vector operations into scalar operations
 * \author Lotus Team
 *
 * This pass eliminates vector load, store, phi, insertelement, and
 * extractelement instructions by expanding them into sequences of scalar
 * instructions. This covers remaining cases that may be left over after running
 * the built-in scalarizer pass.
 */
#pragma once

#include <llvm/IR/Function.h>
#include <llvm/IR/PassManager.h>

using namespace llvm;

/**
 * \class UnrollVectorsPass
 * \brief Vector unrolling pass for converting vector operations to scalar
 * operations
 *
 * This pass expands vector-typed operations into scalar operations, which can
 * be useful for:
 * - Targets that don't support vector instructions
 * - Analysis tools that only handle scalar operations
 * - Further optimization of vector operations by scalar passes
 *
 * The pass handles:
 * - Vector load instructions (expanding into scalar loads)
 * - Vector store instructions (expanding into scalar stores)
 * - Vector PHI nodes (expanding into scalar PHI nodes)
 * - InsertElement instructions (expanding into scalar assignments)
 * - ExtractElement instructions (expanding into scalar loads)
 *
 * Note: The pass does not delete vector operations on its own, but leaves them
 * unused so that dead code elimination can remove them.
 */
class UnrollVectorsPass : public PassInfoMixin<UnrollVectorsPass> {
public:
  /**
   * \brief Run the vector unrolling pass on a function
   * \param F The function to transform
   * \param FAM Function analysis manager
   * \return PreservedAnalyses indicating whether the module was modified
   */
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &FAM);
};

