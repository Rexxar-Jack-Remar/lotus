/**
 * @file RemoveInfiniteLoops.h
 * @brief Pass for removing infinite loops (patterns like LABEL: goto LABEL)
 * @author Migrated from Symbiotic
 */

#ifndef LOTUS_VERIFICATION_TRANSFORM_REMOVE_INFINITE_LOOPS_H
#define LOTUS_VERIFICATION_TRANSFORM_REMOVE_INFINITE_LOOPS_H

#include "llvm/IR/PassManager.h"

namespace llvm {
class Function;
} // namespace llvm

namespace lotus {
namespace verification {
namespace transform {

/**
 * @class RemoveInfiniteLoopsPass
 * @brief Removes infinite loops by replacing them with __VERIFIER_assume(0)
 *
 * This pass detects infinite loops (blocks that unconditionally jump to
 * themselves without side effects) and replaces them with __VERIFIER_assume(0)
 * followed by unreachable, effectively removing the loop.
 */
class RemoveInfiniteLoopsPass
    : public llvm::PassInfoMixin<RemoveInfiniteLoopsPass> {
public:
  llvm::PreservedAnalyses run(llvm::Function &F,
                              llvm::FunctionAnalysisManager &FAM);
};

} // namespace transform
} // namespace verification
} // namespace lotus

#endif // LOTUS_VERIFICATION_TRANSFORM_REMOVE_INFINITE_LOOPS_H
