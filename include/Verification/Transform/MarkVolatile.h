/**
 * @file MarkVolatile.h
 * @brief Pass for marking instructions as volatile based on markers
 * @author Migrated from Symbiotic
 */

#ifndef LOTUS_VERIFICATION_TRANSFORM_MARK_VOLATILE_H
#define LOTUS_VERIFICATION_TRANSFORM_MARK_VOLATILE_H

#include "llvm/IR/PassManager.h"

namespace llvm {
class Function;
} // namespace llvm

namespace lotus {
namespace verification {
namespace transform {

/**
 * @class MarkVolatilePass
 * @brief Makes marked instructions (after __INSTR_mark_* calls) volatile
 *
 * This pass finds calls to __INSTR_mark_* functions and makes the following
 * instruction (load/store/memcpy) volatile, preventing optimizations that
 * might remove or reorder these memory operations.
 */
class MarkVolatilePass : public llvm::PassInfoMixin<MarkVolatilePass> {
public:
  llvm::PreservedAnalyses run(llvm::Function &F,
                              llvm::FunctionAnalysisManager &FAM);
};

} // namespace transform
} // namespace verification
} // namespace lotus

#endif // LOTUS_VERIFICATION_TRANSFORM_MARK_VOLATILE_H
