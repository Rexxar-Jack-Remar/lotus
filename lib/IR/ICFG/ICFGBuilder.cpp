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

          ICFGNode *calleeEntryNode =
              getOrAddIntraBlockICFGNode(&calledFunc->getEntryBlock());
          icfg->addCallEdge(srcNode, calleeEntryNode, call);

          // The return edge should target the return-site block, i.e. the
          // unique successor of the call-site block in the caller's CFG.
          // A call instruction always terminates its own basic block in LLVM
          // IR (the next BB is the normal-return successor at index 0).
          ICFGNode *returnSiteNode = nullptr;
          if (auto *invokeInst = dyn_cast<InvokeInst>(call)) {
            // For invoke, the normal-return destination is the normal dest BB.
            returnSiteNode =
                getOrAddIntraBlockICFGNode(invokeInst->getNormalDest());
          } else {
            // For a regular call, the return site is the block that follows
            // the call-site block (its unique successor).  If the call-site
            // block has no CFG successor (e.g., the call and the ret are in
            // the same single-block function), the return site is the
            // call-site block itself.
            const BasicBlock *callBB = call->getParent();
            if (succ_begin(callBB) != succ_end(callBB))
              returnSiteNode = getOrAddIntraBlockICFGNode(*succ_begin(callBB));
            else
              returnSiteNode = getOrAddIntraBlockICFGNode(callBB);
          }

          if (returnSiteNode) {
            for (inst_iterator I = inst_begin(calledFunc),
                               E = inst_end(calledFunc);
                 I != E; ++I) {
              Instruction &ci = *I;
              if (isa<ReturnInst>(&ci)) {
                ICFGNode *calleeExitNode =
                    getOrAddIntraBlockICFGNode(ci.getParent());
                icfg->addRetEdge(calleeExitNode, returnSiteNode, call);
              }
            }
          }
        }
      }
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
