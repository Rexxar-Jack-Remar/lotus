#include "Alias/TPA/PointerAnalysis/Engine/GlobalState.h"
#include "Alias/TPA/PointerAnalysis/Engine/TransferFunction.h"
#include "Alias/TPA/PointerAnalysis/MemoryModel/MemoryManager.h"
#include "Alias/TPA/PointerAnalysis/MemoryModel/PointerManager.h"

#include <llvm/Support/raw_ostream.h>

using namespace llvm;

namespace tpa {

std::pair<bool, bool>
TransferFunction::evalReturnValue(const context::Context *ctx,
                                  const ReturnCFGNode &retNode,
                                  const ProgramPoint &retSite) {
  assert(retSite.getCFGNode()->isCallNode());
  auto const &callNode =
      static_cast<const CallCFGNode &>(*retSite.getCFGNode());

  const auto *retVal = retNode.getReturnValue();
  if (retVal == nullptr) {
    // Void function: no pointer value is returned. Do NOT write NullObject into
    // the call-site destination — a void return carries no pointer information.
    // Previously this incorrectly wrote {null} into dstVal, which polluted the
    // points-to graph for any call site that (erroneously) has a destination
    // for a void call (e.g., certain invoke lowerings).
    return std::make_pair(true, false);
  }

  const auto *dstVal = callNode.getDest();
  if (dstVal == nullptr)
    // Returned a value, but not used by the caller
    return std::make_pair(true, false);

  auto &ptrManager = globalState.getPointerManager();
  const auto *retPtr = ptrManager.getPointer(ctx, retVal);
  if (retPtr == nullptr)
    // Return value not ready
    return std::make_pair(false, false);

  auto &env = globalState.getEnv();
  auto resSet = env.lookup(retPtr);
  if (resSet.empty())
    // Return pointer not ready
    return std::make_pair(false, false);

  const auto *dstPtr =
      ptrManager.getOrCreatePointer(retSite.getContext(), dstVal);
  return std::make_pair(true, env.weakUpdate(dstPtr, resSet));
}

void TransferFunction::evalReturn(const context::Context *ctx,
                                  const ReturnCFGNode &retNode,
                                  const ProgramPoint &retSite,
                                  EvalResult &evalResult) {
  bool valid, envChanged;
  std::tie(valid, envChanged) = evalReturnValue(ctx, retNode, retSite);

  if (!valid)
    return;
  if (envChanged)
    addTopLevelSuccessors(retSite, evalResult);
  addMemLevelSuccessors(retSite, *localState, evalResult);
}

void TransferFunction::evalReturnNode(const ProgramPoint &pp,
                                      EvalResult &evalResult) {
  const auto *ctx = pp.getContext();
  auto const &retNode = static_cast<const ReturnCFGNode &>(*pp.getCFGNode());

  if (retNode.getFunction().getName() == "main") {
    // Return from main. Do nothing
    // errs() << "Reached program end\n";
    return;
  }

  // Merge back pruned mappings in store
  // auto prunedStore =
  // globalState.getStorePruner().lookupPrunedStore(FunctionContext(ctx,
  // &retNode.getFunction())); if (prunedStore != nullptr)
  //	evalResult.getStore().mergeWith(*prunedStore);

  for (auto retSite : globalState.getCallGraph().getCallers(
           FunctionContext(ctx, &retNode.getFunction())))
    evalReturn(ctx, retNode, retSite, evalResult);
}

} // namespace tpa
