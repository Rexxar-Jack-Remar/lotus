/// @file ICFGBuilder.cpp
/// @brief Implementation of ICFG builder for constructing interprocedural CFG.

#include "IR/ICFG/ICFGBuilder.h"

#include "IR/ICFG/GraphAnalysis.h"

#include <queue>

#include <llvm/IR/Constants.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instructions.h>

using namespace llvm;

/// @brief Builds the ICFG for all non-declaration functions in the module.
void ICFGBuilder::build(llvm::Module *module) {
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

  std::queue<const llvm::BasicBlock *> worklist;
  worklist.push(&func->getEntryBlock());

  std::set<const llvm::BasicBlock *> visited;
  /// function body
  while (!worklist.empty()) {
    auto *bb = worklist.front();
    worklist.pop();

    if (visited.find(bb) == visited.end()) {
      visited.insert(bb);
      ICFGNode *srcNode = getOrAddIntraBlockICFGNode(bb);

      for (auto succIt = succ_begin(bb), e = succ_end(bb); succIt != e;
           ++succIt) {

        auto *succBB = *succIt;
        ICFGNode *dstNode = getOrAddIntraBlockICFGNode(succBB);

        icfg->addIntraEdge(srcNode, dstNode);
        worklist.push(succBB);
      }

      for (auto &i : *bb) {

        if (auto *call = dyn_cast<CallBase>(&i)) {

          Function *calledFunc = call->getCalledFunction();
          if (!calledFunc || calledFunc->isDeclaration())
            continue;

          ICFGNode *calleeEntryNode = icfg->getFunEntryICFGNode(calledFunc);
          icfg->addCallEdge(srcNode, calleeEntryNode, call);

          ICFGNode *returnSiteNode = icfg->getRetICFGNode(call);
          ICFGNode *continuationNode = nullptr;
          if (auto *invokeInst = dyn_cast<InvokeInst>(call)) {
            continuationNode =
                getOrAddIntraBlockICFGNode(invokeInst->getNormalDest());
          } else {
            const BasicBlock *callBB = call->getParent();
            if (succ_begin(callBB) != succ_end(callBB))
              continuationNode =
                  getOrAddIntraBlockICFGNode(*succ_begin(callBB));
          }
          if (returnSiteNode && continuationNode)
            icfg->addIntraEdge(returnSiteNode, continuationNode);

          if (returnSiteNode && funExitNode) {
            for (inst_iterator I = inst_begin(calledFunc),
                               E = inst_end(calledFunc);
                 I != E; ++I) {
              Instruction &ci = *I;
              if (isa<ReturnInst>(&ci)) {
                ICFGNode *calleeExitNode =
                    getOrAddIntraBlockICFGNode(ci.getParent());
                icfg->addIntraEdge(calleeExitNode,
                                   icfg->getFunExitICFGNode(calledFunc));
                icfg->addRetEdge(icfg->getFunExitICFGNode(calledFunc),
                                 returnSiteNode, call);
              }
            }
          }
        }
      }
    }
  }

  (void)funExitNode;
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

    //        if (func->getName() == "quotearg_buffer_restyled") {
    //            outs() << func->getName() << "\n";
    //            for (auto r : res) {
    //
    //                outs() << r->getSrcNode()->toString() << " -> " <<
    //                r->getDstNode()->toString() << "\n";
    //            }
    //        }
  }
}

void ICFGBuilder::removeInterCallCycle() {

  const auto &funcMap = icfg->getFunctionEntryMap();

  for (const auto &p : funcMap) {
    const Function *func = p.first;

    std::set<ICFGEdge *> res;
    findFunctionBackedgesInterICFG(icfg, func, res);

    //        outs() << func->getName() << "\n";
    for (auto *edge : res) {

      icfg->removeICFGEdge(edge);
      //            outs() << r->toString() << "\n";
    }
  }
}

void ICFGBuilder::setRemoveCycleAfterBuild(bool b) {
  _removeCycleAfterBuild = b;
}
