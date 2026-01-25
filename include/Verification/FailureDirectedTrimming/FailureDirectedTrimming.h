/**
 * \file FailureDirectedTrimming.h
 * \brief Failure-directed program trimming (FSE'17) instrumentation pass.
 *
 * Implements the program transformation and lightweight safety condition
 * inference described in "Failure-Directed Program Trimming" (Ferles et al.,
 * ESEC/FSE 2017).
 *
 * The pass instruments LLVM IR with verifier.assume(...) statements that prune
 * execution paths that cannot lead to assertion failure, while preserving
 * equi-safety: the instrumented program has a failing assertion iff the
 * original program has one (under the verification tool's termination model
 * where assert/assume violations terminate execution).
 *
 * The implementation is documented in:
 *   - lib/Verification/FailureDirectedTrimming/README.md
 */
#ifndef VERIFICATION_FAILUREDIRECTEDTRIMMING_FAILUREDIRECTEDTRIMMING_H
#define VERIFICATION_FAILUREDIRECTEDTRIMMING_FAILUREDIRECTEDTRIMMING_H

#include <llvm/IR/Module.h>
#include <llvm/IR/PassManager.h>

using namespace llvm;

/**
 * \class FailureDirectedTrimmingPass
 * \brief LLVM module pass that inserts verifier.assume trimming conditions.
 */
class FailureDirectedTrimmingPass
    : public PassInfoMixin<FailureDirectedTrimmingPass> {
public:
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &MAM);
};

#endif // VERIFICATION_FAILUREDIRECTEDTRIMMING_FAILUREDIRECTEDTRIMMING_H
