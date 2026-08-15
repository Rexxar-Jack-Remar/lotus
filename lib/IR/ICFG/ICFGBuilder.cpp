/// @file ICFGBuilder.cpp
/// @brief Implementation of ICFG builder for constructing interprocedural CFG.

#include "IR/ICFG/ICFGBuilder.h"

#include "IR/ICFG/GraphAnalysis.h"
#include "IR/ICFG/ICFGUtils.h"

#include <queue>
#include <set>

#include <llvm/IR/Constants.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Operator.h>

using namespace llvm;

namespace {

SmallVector<const Function *, 8> collectRootFunctions(Module *module) {
  SmallVector<const Function *, 8> roots;
  if (!module)
    return roots;

  if (const Function *mainFunc = module->getFunction("main")) {
    if (!mainFunc->isDeclaration()) {
      roots.push_back(mainFunc);
      return roots;
    }
  }

  SmallPtrSet<const Function *, 16> definedFuncs;
  SmallPtrSet<const Function *, 16> calledFuncs;
  for (const Function &F : *module) {
    if (F.isDeclaration() || F.isIntrinsic())
      continue;
    definedFuncs.insert(&F);
  }

  for (const Function &F : *module) {
    if (F.isDeclaration() || F.isIntrinsic())
      continue;
    for (const BasicBlock &BB : F) {
      for (const Instruction &I : BB) {
        const auto *call = dyn_cast<CallBase>(&I);
        if (!call)
          continue;
        const Function *callee =
            dyn_cast<Function>(call->getCalledOperand()->stripPointerCasts());
        if (!callee || callee->isDeclaration() || callee->isIntrinsic())
          continue;
        calledFuncs.insert(callee);
      }
    }
  }

  for (const Function *F : definedFuncs) {
    if (!calledFuncs.count(F))
      roots.push_back(F);
  }
  if (!roots.empty())
    return roots;

  for (const Function *F : definedFuncs)
    roots.push_back(F);
  return roots;
}

ICFGCalleeTargets getDefaultCalleeTargets(const CallBase &call) {
  ICFGCalleeTargets result;

  if (const auto *callee =
          dyn_cast<Function>(call.getCalledOperand()->stripPointerCasts())) {
    result.targets.push_back(callee);
    result.complete = true;
    return result;
  }

  // A cheap conservative fallback for unresolved function pointers. Exact
  // function-type matches are useful immediately, while `complete = false`
  // retains the unknown/external summary path for casts or external targets.
  const Module *module = call.getModule();
  if (!module)
    return result;

  for (const Function &candidate : *module) {
    if (candidate.isIntrinsic() ||
        candidate.getFunctionType() != call.getFunctionType())
      continue;
    result.targets.push_back(&candidate);
  }
  return result;
}

} // namespace

/// @brief Builds the ICFG for all non-declaration functions in the module.
void ICFGBuilder::build(llvm::Module *module) {
  ICFGNode *globalInitNode = icfg->getGlobalInitICFGNode();
  for (const Function *root : collectRootFunctions(module)) {
    ICFGNode *entryNode = icfg->getFunEntryICFGNode(root);
    if (globalInitNode && entryNode)
      icfg->addIntraEdge(globalInitNode, entryNode);
  }

  for (auto &func : *module) {
    if (func.isDeclaration() || func.isIntrinsic())
      continue;

    processFunction(&func);
  }

  if (_removeCycleAfterBuild) {

    removeIntraBlockCycle();
    removeInterCallCycle();

    setRemoveCycleAfterBuild(false);
  }
}

void ICFGBuilder::processFunction(const llvm::Function *func) {
  ICFGNode *funEntryNode = icfg->getFunEntryICFGNode(func);
  ICFGNode *entryBlockNode = getOrAddIntraBlockICFGNode(&func->getEntryBlock());
  if (funEntryNode && entryBlockNode)
    icfg->addIntraEdge(funEntryNode, entryBlockNode);
  ICFGNode *funExitNode = icfg->getFunExitICFGNode(func);
  ICFGNode *funUnwindExitNode = nullptr;

  std::queue<const llvm::BasicBlock *> worklist;
  for (const BasicBlock &bb : *func)
    worklist.push(&bb);

  std::set<const llvm::BasicBlock *> visited;
  while (!worklist.empty()) {
    const auto *bb = worklist.front();
    worklist.pop();

    if (visited.find(bb) != visited.end())
      continue;

    visited.insert(bb);
    ICFGNode *srcNode = getOrAddIntraBlockICFGNode(bb);

    for (auto succIt = succ_begin(bb), e = succ_end(bb); succIt != e;
         ++succIt) {
      const auto *succBB = *succIt;
      (void)getOrAddIntraBlockICFGNode(succBB);
      worklist.push(succBB);
    }

    // The basic-block node represents the instruction range before the first
    // call. Each normal return-site node represents the range after that call,
    // so calls in one LLVM block are sequenced without mutating the IR.
    ICFGNode *currentFragmentNode = srcNode;
    for (const Instruction &inst : *bb) {
      const auto *call = dyn_cast<CallBase>(&inst);
      if (!call)
        continue;

      // Instructions following a noreturn call are unreachable on the normal
      // path. They must not acquire edges from the block's entry fragment.
      if (!currentFragmentNode)
        break;

      ICFGNode *returnSiteNode = nullptr;
      ICFGNode *unwindSiteNode = nullptr;
      bool canReturnNormally = !call->doesNotReturn();
      const auto *invokeInst = dyn_cast<InvokeInst>(call);
      bool canUnwind = invokeInst && !call->doesNotThrow();

      if (canReturnNormally)
        returnSiteNode = icfg->getRetICFGNode(call);
      if (canUnwind)
        unwindSiteNode = icfg->getUnwindICFGNode(call);

      ICFGCalleeTargets callees;
      if (const auto *directCallee = dyn_cast<Function>(
              call->getCalledOperand()->stripPointerCasts())) {
        callees.targets.push_back(directCallee);
        callees.complete = true;
      } else {
        callees = calleeProvider ? calleeProvider->getTargets(*call)
                                 : getDefaultCalleeTargets(*call);
      }

      bool hasUnresolvedCallee = !callees.complete;
      for (const Function *calledFunc : callees.targets) {
        if (!calledFunc || calledFunc->isDeclaration()) {
          hasUnresolvedCallee = true;
          break;
        }
      }

      // Summary edges serve intraprocedural clients and also preserve the
      // unknown/external remainder when the target set is incomplete.
      if (returnSiteNode)
        icfg->addCallToRetEdge(currentFragmentNode, returnSiteNode, call,
                               hasUnresolvedCallee);
      if (unwindSiteNode)
        icfg->addCallToRetEdge(currentFragmentNode, unwindSiteNode, call,
                               hasUnresolvedCallee);

      SmallPtrSet<const Function *, 8> seenCallees;
      for (const Function *calledFunc : callees.targets) {
        if (!calledFunc || calledFunc->isDeclaration() ||
            calledFunc->isIntrinsic() || !seenCallees.insert(calledFunc).second)
          continue;

        ICFGNode *calleeEntryNode = icfg->getFunEntryICFGNode(calledFunc);
        if (calleeEntryNode)
          icfg->addCallEdge(currentFragmentNode, calleeEntryNode, call);

        if (returnSiteNode)
          icfg->addRetEdge(icfg->getFunExitICFGNode(calledFunc), returnSiteNode,
                           call);

        if (unwindSiteNode)
          icfg->addExcRetEdge(icfg->getFunUnwindExitICFGNode(calledFunc),
                              unwindSiteNode, call);
      }

      if (invokeInst) {
        if (returnSiteNode)
          icfg->addIntraEdge(returnSiteNode, icfg->getIntraBlockNode(
                                                 invokeInst->getNormalDest()));
        if (unwindSiteNode)
          icfg->addIntraEdge(unwindSiteNode, icfg->getIntraBlockNode(
                                                 invokeInst->getUnwindDest()));
      }

      currentFragmentNode = returnSiteNode;
    }

    const Instruction *terminator = bb->getTerminator();
    if (!currentFragmentNode || isa<InvokeInst>(terminator))
      continue;

    if (isa<ReturnInst>(terminator)) {
      icfg->addIntraEdge(currentFragmentNode, funExitNode);
      continue;
    }

    if (terminator && lotus::icfg::isExceptionalFunctionExitInst(*terminator)) {
      if (!funUnwindExitNode)
        funUnwindExitNode = icfg->getFunUnwindExitICFGNode(func);
      icfg->addIntraEdge(currentFragmentNode, funUnwindExitNode);
    }

    for (auto succIt = succ_begin(bb), e = succ_end(bb); succIt != e;
         ++succIt) {
      ICFGNode *dstNode = icfg->getIntraBlockNode(*succIt);
      icfg->addIntraEdge(currentFragmentNode, dstNode);
    }
  }
}

void ICFGBuilder::removeIntraBlockCycle() {

  const auto &funcMap = icfg->getFunctionEntryMap();

  for (const auto &p : funcMap) {
    const Function *func = p.first;

    std::set<ICFGEdge *> res;
    findFunctionBackedgesIntraICFG(icfg, func, res);

    for (auto *edge : res) {

      icfg->removeICFGEdge(edge);
    }
  }
}

void ICFGBuilder::removeInterCallCycle() {

  const auto &funcMap = icfg->getFunctionEntryMap();

  for (const auto &p : funcMap) {
    const Function *func = p.first;

    std::set<ICFGEdge *> res;
    findFunctionBackedgesInterICFG(icfg, func, res);

    for (auto *edge : res) {

      icfg->removeICFGEdge(edge);
    }
  }
}

void ICFGBuilder::setRemoveCycleAfterBuild(bool b) {
  _removeCycleAfterBuild = b;
}
