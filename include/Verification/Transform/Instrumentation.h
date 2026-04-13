#pragma once

#include "llvm/Pass.h"

namespace lotus {
namespace verification {
namespace transform {

class PrepareOverflowsPass;
class BreakCritLoopsPass;
class RemoveErrorCallsPass;
class RemoveInfiniteLoopsPass;
class MarkVolatilePass;
class DeleteCallsPass;

llvm::Pass *createInitializeUninitializedPass();
llvm::Pass *createMakeNondetPass();
PrepareOverflowsPass createPrepareOverflowsPass();
BreakCritLoopsPass createBreakCritLoopsPass();
llvm::Pass *createDeleteUndefinedPass();
llvm::Pass *createRemoveConstantExprsPass();
llvm::Pass *createInternalizeGlobalsPass();
RemoveErrorCallsPass createRemoveErrorCallsPass();
llvm::Pass *createInstrumentAllocPass();
llvm::Pass *createInstrumentAllocNeverFailsPass();
RemoveInfiniteLoopsPass createRemoveInfiniteLoopsPass();
llvm::Pass *createBreakInfiniteLoopsPass();
llvm::Pass *createFlattenLoopsPass();
llvm::Pass *createInstrumentNonterminationPass();
llvm::Pass *createRemoveReadOnlyAttrPass();
llvm::Pass *createRenameVerifierFunsPass();
llvm::Pass *createReplaceLifetimeMarkersPass();
MarkVolatilePass createMarkVolatilePass();
llvm::Pass *createFindExitsPass();
llvm::Pass *createDummyMarkerPass();
llvm::Pass *createUnrollingPass();
llvm::Pass *createExplicitConsdesPass();
DeleteCallsPass createDeleteCallsPass();
llvm::Pass *createReplaceVerifierAtomicPass();

} // namespace transform
} // namespace verification
} // namespace lotus
