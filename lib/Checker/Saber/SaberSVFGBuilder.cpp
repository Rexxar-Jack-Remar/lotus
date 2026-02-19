//===- SaberSVFGBuilder.cpp -- SVFG builder in Saber-------------------------//
//
// Migrated from SVF's SABER engine to Lotus.
// rmDerefDirSVFGEdges, rmIncomingEdgeForSUStore, AddExtActualParmSVFGNodes
// aligned with SVF semantics using Lotus SVFG APIs.
//
//===----------------------------------------------------------------------===//

#include "Checker/Saber/SaberSVFGBuilder.h"

#include "Checker/Saber/SaberCheckerAPI.h"
#include "Checker/Saber/SaberCondAllocator.h"
#include "Checker/Saber/SaberOptions.h"
#include "IR/ICFG/ICFG.h"
#include "IR/SVFG/SVFG.h"
#include "IR/SVFG/SVFGBase.h"
#include "IR/SVFG/SVFGBuilder.h"
#include "IR/SVFG/SVFGEdge.h"
#include "IR/SVFG/SVFGNode.h"

#include <llvm/IR/GlobalValue.h>
#include <llvm/IR/Instructions.h>

using namespace lotus::analysis;
using namespace llvm;

SVFG *SaberSVFGBuilder::buildSVFG(const ICFG *icfg) {
  SVFGBuilderConfig cfg;
  cfg.resolveIndirectCalls = false;
  cfg.buildMSSA = true;
  SVFG *svfg = build(icfg, cfg);
  currentSVFG_ = svfg;

  if (svfg) {
    collectGlobals();
    rmDerefDirSVFGEdges();
    rmIncomingEdgeForSUStore();
    AddExtActualParmSVFGNodes();
  }

  return svfg;
}

static const Value *getPointerOperandForStmt(const SVFGNode *node) {
  const Instruction *inst = node->getInstruction();
  if (!inst)
    return nullptr;
  if (const auto *si = dyn_cast<StoreInst>(inst))
    return si->getPointerOperand();
  if (const auto *li = dyn_cast<LoadInst>(inst))
    return li->getPointerOperand();
  return nullptr;
}

void SaberSVFGBuilder::rmDerefDirSVFGEdges() {
  if (!currentSVFG_)
    return;
  SVFG *g = currentSVFG_;
  for (auto it = g->begin(), et = g->end(); it != et; ++it) {
    SVFGNode *node = it->second;
    if (!node)
      continue;
    SVFGK k = node->getNodeKind();
    if (k != SVFGK::Store && k != SVFGK::Load)
      continue;

    const Value *ptr = getPointerOperandForStmt(node);
    if (!ptr)
      continue;
    SVFGNode *defNode = g->getValueNode(ptr);
    if (!defNode)
      continue;

    // Match SVF SABER semantics: remove only the direct def->deref edge.
    if (SVFGEdge *edge =
            g->getIntraVFGEdge(defNode, node, SVFGEdgeK::IntraDirect)) {
      g->removeEdge(edge);
    }
    if (accessGlobal(ptr))
      globSVFGNodes.insert(node);
  }
}

void SaberSVFGBuilder::rmIncomingEdgeForSUStore() {
  if (!currentSVFG_ || !saberCondAllocator)
    return;
  SVFG *g = currentSVFG_;
  auto &removed = saberCondAllocator->getRemovedSUVFEdges();
  for (auto it = g->begin(), et = g->end(); it != et; ++it) {
    SVFGNode *node = it->second;
    if (!node || node->getNodeKind() != SVFGK::Store)
      continue;
    uint32_t singleton = 0;
    if (!isStrongUpdate(node, singleton))
      continue;

    std::vector<SVFGEdge *> toRemove;
    for (SVFGEdge *e : node->getInEdges()) {
      if (isIndirectVFGEdge(e->getEdgeKind()))
        toRemove.push_back(e);
    }
    for (SVFGEdge *edge : toRemove) {
      SVFGNode *src = edge->getSrcNode();
      if (src && src->getNodeKind() == SVFGK::Store)
        removed[src].insert(node);
      g->removeEdge(edge);
    }
  }
}

void SaberSVFGBuilder::AddExtActualParmSVFGNodes() {
  if (!currentSVFG_ || !module_)
    return;
  SVFG *g = currentSVFG_;
  SaberCheckerAPI *api = SaberCheckerAPI::getCheckerAPI();
  for (Function &F : *module_) {
    if (F.isDeclaration())
      continue;
    for (BasicBlock &BB : F) {
      for (Instruction &I : BB) {
        auto *cs = dyn_cast<CallBase>(&I);
        if (!cs)
          continue;
        bool sinkLikeCallee = false;
        if (Function *callee = cs->getCalledFunction()) {
          sinkLikeCallee = api->isMemDealloc(callee->getName().str()) ||
                           api->isFClose(callee->getName().str());
        } else {
          for (const Function *c : getIndirectCallTargets(cs)) {
            if (c && (api->isMemDealloc(c->getName().str()) ||
                      api->isFClose(c->getName().str()))) {
              sinkLikeCallee = true;
              break;
            }
          }
        }
        if (!sinkLikeCallee)
          continue;
        unsigned idx = 0;
        for (auto it = cs->arg_begin(), et = cs->arg_end(); it != et;
             ++it, ++idx) {
          Value *arg = it->get();
          if (!arg->getType()->isPointerTy())
            continue;
          SVFGNode *defNode = g->getValueNode(arg);
          if (!defNode)
            continue;
          const auto &parms = g->getActualParms(cs);
          SVFGNode *actualParmNode = nullptr;
          for (SVFGNode *n : parms) {
            if (!n)
              continue;
            if (n->getNodeKind() != SVFGK::ActualParm)
              continue;
            auto *ap = llvm::dyn_cast<ActualParmSVFGNode>(n);
            if (!ap)
              continue;
            if (ap->getParamIndex() == idx) {
              actualParmNode = n;
              break;
            }
          }
          if (!actualParmNode)
            continue;
          if (g->getIntraVFGEdge(defNode, actualParmNode,
                                 SVFGEdgeK::IntraDirect))
            continue;
          g->addEdge(defNode, actualParmNode, SVFGEdgeK::IntraDirect);
        }
      }
    }
  }
}

static uint32_t getBaseObjId(const SVFG *g, uint32_t objId) {
  if (!g || objId == 0)
    return 0;
  if (const auto *info = g->getObjectInfo(objId)) {
    if (info->baseObjId != 0)
      return info->baseObjId;
  }
  return objId;
}

static void collectBaseObjectFields(const SVFG *g, uint32_t baseId,
                                    std::set<uint32_t> &out) {
  if (!g || baseId == 0)
    return;
  out.insert(baseId);
  for (const auto &entry : g->getObjectDebugMap()) {
    uint32_t objId = entry.first;
    const auto *info = g->getObjectInfo(objId);
    if (!info)
      continue;
    if (info->baseObjId == baseId)
      out.insert(objId);
  }
}

static bool isExternalRetObject(const SVFG *g, uint32_t objId) {
  if (!g || objId == 0)
    return false;
  const llvm::Value *v = g->getObjectValue(objId);
  const auto *inst = llvm::dyn_cast<llvm::Instruction>(v);
  if (!inst)
    return false;
  const auto *cs = llvm::dyn_cast<llvm::CallBase>(inst);
  if (!cs)
    return false;
  const llvm::Function *callee = cs->getCalledFunction();
  return callee && callee->isDeclaration();
}

SVFGNodeBS SaberSVFGBuilder::getPointsToOfObject(uint32_t objId) {
  SVFGNodeBS result;
  if (!currentSVFG_ || objId == 0)
    return result;
  const llvm::Value *v = currentSVFG_->getObjectValue(objId);
  if (!v || !v->getType()->isPointerTy())
    return result;
  // When collectExtRetGlobals is false, do not follow points-to from
  // external-return objects (SVF saber-collect-extret-globals: avoids pulling
  // in all malloc-return objects as globals).
  if (!SaberOptions::collectExtRetGlobals()) {
    if (currentSVFG_->isFieldInsensitiveObject(objId) &&
        isExternalRetObject(currentSVFG_, objId))
      return result;
  }
  result = getObjectIdsForValue(v);
  return result;
}

std::set<uint32_t> &
SaberSVFGBuilder::CollectPtsChain(uint32_t id, NodeToPTSSMap &cachedPtsMap) {
  uint32_t baseId = getBaseObjId(currentSVFG_, id);
  auto it = cachedPtsMap.find(baseId);
  if (it != cachedPtsMap.end())
    return it->second;

  std::set<uint32_t> &pts = cachedPtsMap[baseId];
  if (!SaberOptions::collectExtRetGlobals()) {
    if (currentSVFG_ && currentSVFG_->isFieldInsensitiveObject(baseId) &&
        isExternalRetObject(currentSVFG_, baseId))
      return pts;
  }

  collectBaseObjectFields(currentSVFG_, baseId, pts);
  WorkList worklist;
  for (uint32_t objId : pts)
    worklist.push_back(objId);

  while (!worklist.empty()) {
    uint32_t objId = worklist.front();
    worklist.pop_front();
    SVFGNodeBS directPts = getPointsToOfObject(objId);
    for (uint32_t o : directPts) {
      std::set<uint32_t> &sub = CollectPtsChain(o, cachedPtsMap);
      for (uint32_t subId : sub) {
        if (pts.insert(subId).second)
          worklist.push_back(subId);
      }
    }
  }
  return pts;
}

void SaberSVFGBuilder::collectGlobals() {
  globs.clear();
  globSVFGNodes.clear();
  if (!currentSVFG_)
    return;

  std::set<uint32_t> seedGlobals;
  for (const auto &entry : currentSVFG_->getObjectDebugMap()) {
    uint32_t objId = entry.first;
    if (currentSVFG_->isGlobalObject(objId))
      seedGlobals.insert(objId);
  }

  // Also seed from global LLVM values (in case they are not yet in the graph).
  if (module_) {
    for (const llvm::GlobalVariable &g : module_->globals()) {
      uint32_t objId = currentSVFG_->getObjectId(&g);
      if (objId != 0 && currentSVFG_->isGlobalObject(objId))
        seedGlobals.insert(objId);
    }
  }

  NodeToPTSSMap cachedPtsMap;
  for (uint32_t id : seedGlobals) {
    std::set<uint32_t> &chain = CollectPtsChain(id, cachedPtsMap);
    globs.insert(chain.begin(), chain.end());
  }
}

void SaberSVFGBuilder::recomputeGlobalSVFGNodes() {
  globSVFGNodes.clear();
  if (!currentSVFG_)
    return;

  for (auto it = currentSVFG_->begin(), et = currentSVFG_->end(); it != et;
       ++it) {
    SVFGNode *node = it->second;
    if (!node)
      continue;
    const SVFGK k = node->getNodeKind();
    if (k != SVFGK::Store && k != SVFGK::Load)
      continue;
    const Value *ptr = getPointerOperandForStmt(node);
    if (ptr && accessGlobal(ptr))
      globSVFGNodes.insert(node);
  }
}

bool SaberSVFGBuilder::accessGlobal(const SVFGNode *node) {
  if (!node || !currentSVFG_)
    return false;
  const Value *ptr = getPointerOperandForStmt(node);
  if (ptr)
    return accessGlobal(ptr);
  const Value *v = node->getValue();
  return v && accessGlobal(v);
}

bool SaberSVFGBuilder::accessGlobal(const llvm::Value *ptr) {
  if (!ptr || !currentSVFG_)
    return false;
  if (const auto *gv = llvm::dyn_cast<llvm::GlobalValue>(ptr)) {
    uint32_t objId = currentSVFG_->getObjectId(gv);
    if (objId != 0 && globs.count(objId))
      return true;
  }
  SVFGNodeBS objIds = getObjectIdsForValue(ptr);
  for (uint32_t objId : objIds) {
    if (globs.count(objId))
      return true;
  }
  return false;
}

bool SaberSVFGBuilder::isStrongUpdate(const SVFGNode *node,
                                      uint32_t &singleton) {
  if (!node || !currentSVFG_)
    return false;

  if (node->getNodeKind() != SVFGK::Store)
    return false;

  const Value *storePtr = getPointerOperandForStmt(node);
  if (!storePtr)
    return false;

  SVFGNodeBS objIds = getObjectIdsForValue(storePtr);
  if (objIds.size() != 1)
    return false;

  uint32_t objId = *objIds.begin();
  if (!currentSVFG_->isHeapObject(objId) &&
      !currentSVFG_->isArrayObject(objId)) {
    if (const auto *info = currentSVFG_->getObjectInfo(objId)) {
      if (!info->isFieldInsensitive) {
        singleton = objId;
        return true;
      }
    }
  }

  return false;
}
