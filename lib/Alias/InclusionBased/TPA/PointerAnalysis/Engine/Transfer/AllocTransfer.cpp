#include "Alias/InclusionBased/TPA/PointerAnalysis/Engine/GlobalState.h"
#include "Alias/InclusionBased/TPA/PointerAnalysis/Engine/TransferFunction.h"
#include "Alias/InclusionBased/TPA/PointerAnalysis/MemoryModel/MemoryManager.h"
#include "Alias/InclusionBased/TPA/PointerAnalysis/MemoryModel/PointerManager.h"

namespace tpa {

bool TransferFunction::evalMemoryAllocation(const context::Context *ctx,
                                            const llvm::Instruction *inst,
                                            const TypeLayout *type,
                                            bool isHeap) {
  const auto *ptr =
      globalState.getPointerManager().getOrCreatePointer(ctx, inst);

  const auto *mem =
      isHeap
          ? globalState.getMemoryManager().allocateHeapMemory(ctx, inst, type)
          : globalState.getMemoryManager().allocateStackMemory(ctx, inst, type);

  return globalState.getEnv().strongUpdate(ptr, PtsSet::getSingletonSet(mem));
}

void TransferFunction::evalAllocNode(const ProgramPoint &pp,
                                     EvalResult &evalResult) {
  const auto *allocNode = static_cast<const AllocCFGNode *>(pp.getCFGNode());
  auto envChanged =
      evalMemoryAllocation(pp.getContext(), allocNode->getDest(),
                           allocNode->getAllocTypeLayout(), false);

  if (envChanged)
    addTopLevelSuccessors(pp, evalResult);
}

} // namespace tpa
