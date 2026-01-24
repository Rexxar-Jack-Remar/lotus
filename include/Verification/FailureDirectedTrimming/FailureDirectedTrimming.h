/**
 * \file FailureDirectedTrimming.h
 * \brief Failure-directed program trimming (FSE'17) instrumentation pass.
 *
 * Implements the program transformation and lightweight safety condition
 * inference described in "Failure-Directed Program Trimming" (Ferles et al.,
 * ESEC/FSE 2017).
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
