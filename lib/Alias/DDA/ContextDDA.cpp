//===- ContextDDA.cpp -- Context-sensitive DDA (SVF-style) ----------------//
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
//===----------------------------------------------------------------------===//

#include "Alias/DDA/ContextDDA.h"
#include "Alias/DDA/DPItem.h"
#include "IR/SVFG/SVFGEdge.h"
#include "IR/SVFG/SVFGNode.h"

#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/Casting.h>

#include <functional>
#include <stack>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace lotus::analysis;
using namespace llvm;

namespace {

static bool isDirectEdge(SVFGEdge *e) {
  if (!e) return false;
  SVFGEdgeK k = e->getEdgeKind();
  return k == SVFGEdgeK::IntraCopy || k == SVFGEdgeK::IntraDirect ||
         k == SVFGEdgeK::IntraPhi || k == SVFGEdgeK::IntraGep ||
         k == SVFGEdgeK::CallDir || k == SVFGEdgeK::RetDir ||
         k == SVFGEdgeK::ParamCall || k == SVFGEdgeK::ParamRet ||
         k == SVFGEdgeK::IntraCmp || k == SVFGEdgeK::IntraBranch;
}

static bool isIndirectEdge(SVFGEdge *e) {
  if (!e) return false;
  SVFGEdgeK k = e->getEdgeKind();
  return k == SVFGEdgeK::IntraLoad || k == SVFGEdgeK::IntraStore ||
         k == SVFGEdgeK::IntraMu || k == SVFGEdgeK::IntraChi ||
         k == SVFGEdgeK::IntraIndirect || k == SVFGEdgeK::CallInd ||
         k == SVFGEdgeK::RetInd || k == SVFGEdgeK::ThreadMHPIndirectVF;
}

static bool isCallEdge(SVFGEdge *e) {
  if (!e) return false;
  SVFGEdgeK k = e->getEdgeKind();
  return k == SVFGEdgeK::CallDir || k == SVFGEdgeK::CallInd ||
         k == SVFGEdgeK::ParamCall;
}

static bool isRetEdge(SVFGEdge *e) {
  if (!e) return false;
  SVFGEdgeK k = e->getEdgeKind();
  return k == SVFGEdgeK::RetDir || k == SVFGEdgeK::RetInd ||
         k == SVFGEdgeK::ParamRet;
}

static const llvm::Value *getValueForCxtVar(const CxtLocDPItem &dpm, SVFG *svfg) {
  if (!svfg)
    return nullptr;
  if (const llvm::Value *objVal = svfg->getObjectValue(dpm.getCurNodeID()))
    return objVal;
  if (const SVFGNode *loc = dpm.getLoc())
    return loc->getValue();
  return nullptr;
}

} // namespace

ContextDDA::ContextDDA(DemandDrivenAA *flowDDA, DDAClient *client)
    : flowDDA_(flowDDA), client_(client) {}

ContextDDA::~ContextDDA() = default;

bool ContextDDA::run(llvm::Module &M) {
  if (!flowDDA_ || !flowDDA_->run(M))
    return false;
  buildRecursionInfo();
  return true;
}

uint32_t ContextDDA::getOrCreateCSID(const llvm::CallBase *cs) {
  if (!cs) return 0;
  auto it = callSiteToId_.find(cs);
  if (it != callSiteToId_.end())
    return it->second;
  uint32_t id = nextCallSiteId_++;
  callSiteToId_[cs] = id;
  return id;
}

uint32_t ContextDDA::getCSIDAtCall(CxtLocDPItem &, SVFGEdge *edge) {
  (void)edge;
  const llvm::CallBase *cs = edge->getCallSite();
  return getOrCreateCSID(cs);
}

uint32_t ContextDDA::getCSIDAtRet(CxtLocDPItem &, SVFGEdge *edge) {
  const llvm::CallBase *cs = edge->getCallSite();
  return getOrCreateCSID(cs);
}

bool ContextDDA::isEdgeInRecursion(uint32_t csId) const {
  return recursiveCallSiteIds_.count(csId) != 0;
}

void ContextDDA::popRecursiveCallSites(CxtLocDPItem &dpm) {
  while (!dpm.getCond().getContexts().empty() &&
         isEdgeInRecursion(dpm.getCond().getContexts().back()))
    dpm.getCond().popBack();
}

bool ContextDDA::handleBKCondition(CxtLocDPItem &dpm, SVFGEdge *edge) {
  if (client_)
    client_->handleStatement(edge->getSrcNode(), dpm.getCurNodeID());
  SVFG *svfg = getSVFG();
  if (!svfg) return true;
  if (isCallEdge(edge)) {
    uint32_t csId = getCSIDAtCall(dpm, edge);
    if (csId != 0) {
      if (isEdgeInRecursion(csId))
        popRecursiveCallSites(dpm);
      if (!dpm.matchContext(csId))
        return false;
    }
  } else if (isRetEdge(edge)) {
    uint32_t csId = getCSIDAtRet(dpm, edge);
    if (csId != 0) {
      if (dpm.getCond().containCallStr(csId))
        return false;
      dpm.pushContext(csId);
    }
  }
  return true;
}

void ContextDDA::handleOutOfBudgetDpm(const CxtLocDPItem &dpm) {
  // Match SVF ContextDDA::handleOutOfBudgetDpm: downgrade to FlowDDA
  if (!flowDDA_ || !getSVFG())
    return;
  
  const llvm::Value *v = getValueForCxtVar(dpm, getSVFG());
  if (!v || !v->getType()->isPointerTy())
    return;

  DemandDrivenAA::PtsSet flowPts = flowDDA_->getPointsTo(v);
  
  // Convert to context-sensitive points-to set
  CxtPtSet cxtPts;
  ContextCond cond = dpm.getCond();
  for (const llvm::Value *v : flowPts) {
    uint32_t objId = getSVFG()->getObjectId(v);
    if (objId != 0) {
      CxtVar var(cond, objId);
      cxtPts.insert(var);
    }
  }
  
  // Update cache with conservative points-to
  updateCachedPointsTo(dpm, cxtPts);
}

void ContextDDA::buildRecursionInfo() {
  recursiveCallSiteIds_.clear();
  const llvm::Module *module = flowDDA_ ? flowDDA_->getModule() : nullptr;
  if (!module)
    return;
  std::unordered_map<const llvm::Function *,
                     std::vector<const llvm::Function *>>
      callGraph;
  for (const llvm::Function &F : *module) {
    if (F.isDeclaration())
      continue;
    for (const llvm::BasicBlock &BB : F)
      for (const llvm::Instruction &I : BB) {
        const llvm::CallBase *cb = llvm::dyn_cast<llvm::CallBase>(&I);
        if (!cb)
          continue;
        const llvm::Function *callee = cb->getCalledFunction();
        if (!callee || callee->isDeclaration())
          continue;
        callGraph[&F].push_back(callee);
      }
  }
  std::unordered_map<const llvm::Function *, uint32_t> index, lowlink;
  std::stack<const llvm::Function *> stk;
  std::unordered_set<const llvm::Function *> onStack;
  uint32_t nextIndex = 0;
  std::vector<std::set<const llvm::Function *>> sccs;

  std::function<void(const llvm::Function *)> strongConnect;
  strongConnect = [&](const llvm::Function *f) {
    index[f] = lowlink[f] = nextIndex++;
    stk.push(f);
    onStack.insert(f);
    auto it = callGraph.find(f);
    if (it != callGraph.end()) {
      for (const llvm::Function *callee : it->second) {
        if (index.count(callee) == 0) {
          strongConnect(callee);
          lowlink[f] = std::min(lowlink[f], lowlink[callee]);
        } else if (onStack.count(callee))
          lowlink[f] = std::min(lowlink[f], index[callee]);
      }
    }
    if (lowlink[f] == index[f]) {
      std::set<const llvm::Function *> scc;
      const llvm::Function *w;
      do {
        w = stk.top();
        stk.pop();
        onStack.erase(w);
        scc.insert(w);
      } while (w != f);
      sccs.push_back(std::move(scc));
    }
  };

  for (const llvm::Function &F : *module) {
    if (F.isDeclaration())
      continue;
    if (index.count(&F) == 0)
      strongConnect(&F);
  }

  std::unordered_map<const llvm::Function *, size_t> funcToScc;
  for (size_t i = 0; i < sccs.size(); ++i)
    for (const llvm::Function *f : sccs[i])
      funcToScc[f] = i;

  for (const llvm::Function &F : *module) {
    if (F.isDeclaration())
      continue;
    for (const llvm::BasicBlock &BB : F)
      for (const llvm::Instruction &I : BB) {
        const llvm::CallBase *cb = llvm::dyn_cast<llvm::CallBase>(&I);
        if (!cb)
          continue;
        const llvm::Function *callee = cb->getCalledFunction();
        if (!callee || callee->isDeclaration())
          continue;
        auto itCaller = funcToScc.find(&F);
        auto itCallee = funcToScc.find(callee);
        if (itCaller == funcToScc.end() || itCallee == funcToScc.end())
          continue;
        size_t sccId = itCaller->second;
        if (itCallee->second != sccId || sccs[sccId].size() <= 1)
          continue;
        uint32_t csId = getOrCreateCSID(cb);
        if (csId != 0)
          recursiveCallSiteIds_.insert(csId);
      }
  }
}

void ContextDDA::resetQuery() {
  backwardVisited_.clear();
  dpmToPtsMap_.clear();
  locToDpmSetMap_.clear();
  dpmToLoadDpmMap_.clear();
  dpmToLoadCVarMap_.clear();
  numSteps_ = 0;
  outOfBudget_ = false;
}

void ContextDDA::markbkVisited(const CxtLocDPItem &dpm) {
  backwardVisited_.insert(dpm);
}

bool ContextDDA::isbkVisited(const CxtLocDPItem &dpm) const {
  return backwardVisited_.count(dpm) != 0;
}

void ContextDDA::clearbkVisited(const CxtLocDPItem &dpm) {
  backwardVisited_.erase(dpm);
}

const CxtPtSet &ContextDDA::getCachedPointsTo(const CxtLocDPItem &dpm) const {
  auto it = dpmToPtsMap_.find(dpm);
  static const CxtPtSet empty;
  return (it != dpmToPtsMap_.end()) ? it->second : empty;
}

void ContextDDA::updateCachedPointsTo(const CxtLocDPItem &dpm, const CxtPtSet &pts) {
  // Match SVF DDAVFSolver::updateCachedPointsTo: only recompute if pts grew
  auto it = dpmToPtsMap_.find(dpm);
  if (it == dpmToPtsMap_.end()) {
    dpmToPtsMap_[dpm] = pts;
    // No need to recompute for new entry
    return;
  }
  size_t oldSize = it->second.size();
  for (const CxtVar &v : pts)
    it->second.insert(v);
  if (it->second.size() != oldSize) {
    // Points-to grew: recompute successors
    reCompute(dpm);
  }
}

CxtPtSet ContextDDA::getConservativeCPts(const CxtLocDPItem &dpm) const {
  // Match SVF ContextDDA::getConservativeCPts: downgrade to FlowDDA
  if (!flowDDA_ || !getSVFG())
    return CxtPtSet{};
  
  const llvm::Value *v = getValueForCxtVar(dpm, getSVFG());
  if (!v || !v->getType()->isPointerTy())
    return CxtPtSet{};

  DemandDrivenAA::PtsSet flowPts = flowDDA_->getPointsTo(v);
  CxtPtSet cxtPts;
  ContextCond cond = dpm.getCond();
  for (const llvm::Value *v : flowPts) {
    uint32_t objId = getSVFG()->getObjectId(v);
    if (objId != 0) {
      CxtVar var(cond, objId);
      cxtPts.insert(var);
    }
  }
  return cxtPts;
}

void ContextDDA::addLoadDpmAndCVar(const CxtLocDPItem &dpm,
                                   const CxtLocDPItem &loadDpm,
                                   uint32_t loadCVarObjId) {
  auto it = dpmToLoadDpmMap_.find(dpm);
  if (it != dpmToLoadDpmMap_.end())
    it->second = loadDpm;
  else
    dpmToLoadDpmMap_.emplace(dpm, loadDpm);
  dpmToLoadCVarMap_[dpm] = loadCVarObjId;
}

bool ContextDDA::hasLoadDpm(const CxtLocDPItem &dpm) const {
  return dpmToLoadDpmMap_.find(dpm) != dpmToLoadDpmMap_.end();
}

CxtLocDPItem ContextDDA::getLoadDpm(const CxtLocDPItem &dpm) const {
  auto it = dpmToLoadDpmMap_.find(dpm);
  if (it != dpmToLoadDpmMap_.end())
    return it->second;
  return dpm;
}

uint32_t ContextDDA::getLoadCVar(const CxtLocDPItem &dpm) const {
  auto it = dpmToLoadCVarMap_.find(dpm);
  return (it != dpmToLoadCVarMap_.end()) ? it->second : dpm.getCurNodeID();
}

bool ContextDDA::isMustAlias(const CxtLocDPItem &loadDpm,
                            const CxtLocDPItem &storeDpm) const {
  auto it1 = dpmToPtsMap_.find(loadDpm);
  auto it2 = dpmToPtsMap_.find(storeDpm);
  if (it1 == dpmToPtsMap_.end() || it2 == dpmToPtsMap_.end())
    return false;
  const CxtPtSet &loadPts = it1->second;
  const CxtPtSet &storePts = it2->second;
  return loadPts.size() == 1 && storePts.size() == 1 &&
         *loadPts.begin() == *storePts.begin();
}

bool ContextDDA::propagateViaObj(uint32_t storeObjId, uint32_t loadCVarObjId) const {
  return storeObjId == loadCVarObjId;
}

SVFGNode *ContextDDA::getDefNodeForValue(const llvm::Value *v) const {
  SVFG *svfg = getSVFG();
  if (!svfg || !v) return nullptr;
  if (SVFGNode *n = svfg->getValueNode(v))
    return n;
  if (const Instruction *inst = llvm::dyn_cast<Instruction>(v))
    return svfg->getDef(inst);
  return nullptr;
}

CxtLocDPItem ContextDDA::getDPImWithOldCond(const CxtLocDPItem &oldDpm,
                                             uint32_t objId,
                                             const SVFGNode *loc) const {
  CxtVar var(oldDpm.getCond(), objId);
  CxtLocDPItem dpm(var, loc);
  // Match SVF DDAVFSolver::getDPImWithOldCond: add load info for Store/Load nodes.
  ContextDDA *nonConstThis = const_cast<ContextDDA *>(this);
  if (llvm::isa<StoreSVFGNode>(loc))
    nonConstThis->addLoadDpmAndCVar(dpm, getLoadDpm(oldDpm), objId);
  if (llvm::isa<LoadSVFGNode>(loc))
    nonConstThis->addLoadDpmAndCVar(dpm, oldDpm, objId);
  return dpm;
}

void ContextDDA::handleAddr(CxtPtSet &pts, const CxtLocDPItem &dpm,
                            const AddrSVFGNode *addr) {
  ContextCond cond = dpm.getCond();
  SVFG *svfg = getSVFG();
  uint32_t objId = (svfg && addr->getValue()) ? svfg->getObjectId(addr->getValue()) : 0;
  if (objId == 0)
    objId = addr->getId();
  pts.insert(CxtVar(cond, objId));
}

void ContextDDA::backwardPropDpm(CxtPtSet &pts, uint32_t ptrNodeId,
                                 const CxtLocDPItem &oldDpm, SVFGEdge *edge) {
  SVFG *svfg = getSVFG();
  SVFGNode *src = edge->getSrcNode();
  if (!src || !svfg) return;
  CxtLocDPItem dpm(oldDpm);
  dpm.setLocVar(src, ptrNodeId);
  if (!handleBKCondition(dpm, edge))
    return;
  // Match SVF DDAVFSolver: record load dpm/cvar when crossing indirect edge.
  // SVF always adds when crossing indirect edge (doesn't check hasLoadDpm)
  if (isIndirectEdge(edge))
    addLoadDpmAndCVar(dpm, getLoadDpm(oldDpm), getLoadCVar(oldDpm));
  // Match SVF: use unionDDAPts pattern
  const CxtPtSet &predPts = findPT(dpm);
  for (const CxtVar &v : predPts)
    pts.insert(v);
}

void ContextDDA::backtraceAlongDirectVF(CxtPtSet &pts,
                                        const CxtLocDPItem &oldDpm) {
  const SVFGNode *node = oldDpm.getLoc();
  SVFG *svfg = getSVFG();
  if (!node || !svfg) return;
  for (SVFGEdge *edge : node->getInEdges()) {
    if (!isDirectEdge(edge)) continue;
    SVFGNode *src = edge->getSrcNode();
    SVFGNode *lhs = svfg->getLHSTopLevPtr(src);
    if (lhs)
      backwardPropDpm(pts, lhs->getId(), oldDpm, edge);
  }
}

void ContextDDA::backtraceAlongIndirectVF(CxtPtSet &pts,
                                          const CxtLocDPItem &oldDpm,
                                          const CxtPtSet &curObjPts) {
  (void)curObjPts;
  const SVFGNode *node = oldDpm.getLoc();
  SVFG *svfg = getSVFG();
  if (!node || !svfg) return;
  uint32_t obj = oldDpm.getCurNodeID();
  // Match SVF DDAVFSolver: skip constant objects (obj == 0 represents null/constant).
  if (obj == 0)
    return;
  for (SVFGEdge *edge : node->getInEdges()) {
    if (!isIndirectEdge(edge)) continue;
    // Match SVF DDAVFSolver::backtraceAlongIndirectVF: only follow indirect edge
    // when obj is in guard (pointsTo set). In SVF: guard.test(obj) checks membership.
    const std::set<uint32_t> &guard = edge->getPointsTo();
    // If guard is empty, guard.test(obj) would return false, so don't follow.
    // If guard is not empty, only follow if obj is in guard.
    if (guard.empty() || guard.count(obj) == 0)
      continue;
    SVFGNode *src = edge->getSrcNode();
    SVFGNode *lhs = svfg->getLHSTopLevPtr(src);
    if (lhs)
      backwardPropDpm(pts, lhs->getId(), oldDpm, edge);
  }
}

void ContextDDA::startNewPTCompFromLoadSrc(CxtPtSet &loadPts,
                                           const CxtLocDPItem &oldDpm) {
  const LoadSVFGNode *load = llvm::cast<LoadSVFGNode>(oldDpm.getLoc());
  SVFG *svfg = getSVFG();
  if (!svfg) return;
  uint32_t ptrNodeId = load->getLoadFromPtr();
  SVFGNode *loadSrc = svfg->getNode(ptrNodeId);
  if (!loadSrc) return;
  SVFGEdge *edge = svfg->getIntraVFGEdge(loadSrc, load, SVFGEdgeK::IntraDirect);
  if (edge)
    backwardPropDpm(loadPts, ptrNodeId, oldDpm, edge);
}

void ContextDDA::startNewPTCompFromStoreDst(CxtPtSet &storePts,
                                            const CxtLocDPItem &oldDpm) {
  const StoreSVFGNode *store = llvm::cast<StoreSVFGNode>(oldDpm.getLoc());
  SVFG *svfg = getSVFG();
  if (!svfg) return;
  uint32_t ptrNodeId = store->getStoreToPtr();
  SVFGNode *storeDst = svfg->getNode(ptrNodeId);
  if (!storeDst) return;
  SVFGEdge *edge =
      svfg->getIntraVFGEdge(storeDst, store, SVFGEdgeK::IntraDirect);
  if (edge)
    backwardPropDpm(storePts, ptrNodeId, oldDpm, edge);
}

void ContextDDA::backtraceToStoreSrc(CxtPtSet &pts, const CxtLocDPItem &oldDpm) {
  const StoreSVFGNode *store = llvm::cast<StoreSVFGNode>(oldDpm.getLoc());
  const Value *valueOperand =
      llvm::cast<StoreInst>(store->getValue())->getValueOperand();
  SVFGNode *storeSrc = getDefNodeForValue(valueOperand);
  if (!storeSrc) return;
  SVFG *svfg = getSVFG();
  if (!svfg) return;
  SVFGEdge *edge =
      svfg->getIntraVFGEdge(storeSrc, store, SVFGEdgeK::IntraDirect);
  if (edge)
    backwardPropDpm(pts, storeSrc->getId(), oldDpm, edge);
}

CxtPtSet ContextDDA::processGepPts(const GepSVFGNode *gep,
                                    const CxtPtSet &srcPts) {
  if (!gep || !gep->getValue() || !isa<GetElementPtrInst>(gep->getValue()))
    return srcPts;
  
  const auto *gi = cast<GetElementPtrInst>(gep->getValue());
  CxtPtSet tmpDstPts;
  
  // Match SVF ContextDDA::processGepPts logic
  for (const CxtVar &ptd : srcPts) {
    // Check if this is a block object or constant (pass through)
    if (ptd.get_id() == 0) {
      tmpDstPts.insert(ptd);
      continue;
    }
    
    // Handle field-sensitive GEP
    bool isVariantFieldGep = false;
    if (gi->hasIndices()) {
      // Check if any index is not constant
      for (unsigned i = 1; i < gi->getNumOperands(); ++i) {
        if (!isa<ConstantInt>(gi->getOperand(i))) {
          isVariantFieldGep = true;
          break;
        }
      }
    }
    
    if (isVariantFieldGep) {
      // Variant field GEP: pass through with same context (field-insensitive)
      tmpDstPts.insert(ptd);
    } else {
      // Constant field GEP: create field object with same context
      // TODO: Implement proper field-sensitive object creation
      tmpDstPts.insert(ptd);
    }
  }
  
  return tmpDstPts;
}

bool ContextDDA::isStrongUpdate(const CxtPtSet &dstPts,
                                const StoreSVFGNode *store) {
  if (dstPts.size() != 1)
    return false;
  
  // Match SVF ContextDDA::isStrongUpdate logic
  // In SVF, this checks isHeapCondMemObj, isArrayCondMemObj, isFieldInsenCondMemObj,
  // and isLocalCVarInRecursion
  
  const CxtVar &var = *dstPts.begin();
  uint32_t objId = var.get_id();
  
  // Check for heap objects
  SVFG *svfg = getSVFG();
  if (svfg) {
    const Value *v = svfg->getObjectValue(objId);
    if (v) {
      // Check if it's a heap allocation
      if (const CallBase *cb = dyn_cast<CallBase>(v)) {
        const Function *callee = cb->getCalledFunction();
        if (callee) {
          StringRef name = callee->getName();
          if (name == "malloc" || name == "calloc" || name == "realloc" ||
              name == "_Znam" || name == "_Znwm" || name == "aligned_alloc") {
            return false;
          }
        }
      }
    }
    
    // Check for array objects
    if (v && v->getType()->isPointerTy()) {
      if (const Type *elemTy = v->getType()->getPointerElementType()) {
        if (elemTy->isArrayTy())
          return false;
      }
    }
  }
  
  // Check for local variables in recursion
  // TODO: Implement isLocalCVarInRecursion check for context-sensitive case
  
  return true;
}

void ContextDDA::handleSingleStatement(const CxtLocDPItem &dpm, CxtPtSet &pts) {
  const SVFGNode *node = dpm.getLoc();
  SVFG *svfg = getSVFG();
  if (!node || !svfg) return;
  switch (node->getNodeKind()) {
  case SVFGK::Addr:
    handleAddr(pts, dpm, llvm::cast<AddrSVFGNode>(node));
    break;
  case SVFGK::Copy:
  case SVFGK::Phi:
  case SVFGK::IntraPhi:
  case SVFGK::InterPhi:
  case SVFGK::FormalParm:
  case SVFGK::ActualParm:
  case SVFGK::FormalRet:
  case SVFGK::ActualRet:
  case SVFGK::NullPtr:
    backtraceAlongDirectVF(pts, dpm);
    break;
  case SVFGK::Gep: {
    CxtPtSet gepPts;
    backtraceAlongDirectVF(gepPts, dpm);
    CxtPtSet filtered = processGepPts(llvm::cast<GepSVFGNode>(node), gepPts);
    for (const CxtVar &v : filtered)
      pts.insert(v);
    break;
  }
  case SVFGK::Load: {
    const LoadSVFGNode *load = llvm::cast<LoadSVFGNode>(node);
    if (!load->getValue() || !load->getValue()->getType()->isPointerTy())
      break;
    CxtPtSet loadPts;
    startNewPTCompFromLoadSrc(loadPts, dpm);
    for (const CxtVar &v : loadPts) {
      CxtLocDPItem objDpm = getDPImWithOldCond(dpm, v.get_id(), load);
      // getDPImWithOldCond already adds loadDpm/loadCVar for Load nodes.
      backtraceAlongIndirectVF(pts, objDpm, CxtPtSet{});
    }
    break;
  }
  case SVFGK::Store: {
    const StoreSVFGNode *store = llvm::cast<StoreSVFGNode>(node);
    if (!store->getValue() || !store->getValue()->getType()->isPointerTy())
      break;
    if (hasLoadDpm(dpm) && isMustAlias(getLoadDpm(dpm), dpm)) {
      backtraceToStoreSrc(pts, dpm);
      break;
    }
    CxtPtSet storePts;
    startNewPTCompFromStoreDst(storePts, dpm);
    if (!storePts.empty()) {
      // Match SVF DDAVFSolver Store handling: for each store target, check propagateViaObj.
      for (const CxtVar &v : storePts) {
        if (propagateViaObj(v.get_id(), getLoadCVar(dpm))) {
          CxtLocDPItem objDpm = getDPImWithOldCond(dpm, v.get_id(), store);
          // getDPImWithOldCond already adds loadDpm/loadCVar for Store nodes.
          backtraceToStoreSrc(pts, objDpm);
          // Check strong update: if not strong, also do indirect backtrace.
          if (!isStrongUpdate(storePts, store))
            backtraceAlongIndirectVF(pts, objDpm, CxtPtSet{});
        } else {
          // When propagateViaObj is false, use original dpm (not objDpm).
          backtraceAlongIndirectVF(pts, dpm, CxtPtSet{});
        }
      }
    }
    break;
  }
  default:
    if (node->isMemNode()) {
      backtraceAlongIndirectVF(pts, dpm, CxtPtSet{});
    }
    break;
  }
}

const CxtPtSet &ContextDDA::findPT(const CxtLocDPItem &dpm) {
  // Match SVF ContextDDA::findPT: check cache first
  if (isbkVisited(dpm)) {
    const CxtPtSet &cpts = getCachedPointsTo(dpm);
    return cpts;
  }

  // Mark as visited and add to location map
  markbkVisited(dpm);
  if (dpm.getLoc())
    locToDpmSetMap_[dpm.getLoc()->getId()].insert(dpm);

  // Check out-of-budget
  numSteps_++;
  if (numSteps_ > kDefaultMaxBudget) {
    outOfBudget_ = true;
    handleOutOfBudgetDpm(dpm);
    // Return conservative points-to if available, otherwise empty
    CxtPtSet conservativePts = getConservativeCPts(dpm);
    if (!conservativePts.empty()) {
      dpmToPtsMap_[dpm] = conservativePts;
    } else {
      dpmToPtsMap_.emplace(dpm, CxtPtSet{});
    }
    return dpmToPtsMap_.find(dpm)->second;
  }

  // Compute points-to for this dpm
  CxtPtSet pts;
  handleSingleStatement(dpm, pts);
  
  // Update cache and recompute if needed
  updateCachedPointsTo(dpm, pts);
  return getCachedPointsTo(dpm);
}

const CxtPtSet &ContextDDA::computeDDAPts(const CxtVar &cxtVar) {
  resetQuery();
  DPItem::setMaxBudget(kDefaultMaxBudget);
  SVFG *svfg = getSVFG();
  if (!svfg) {
    static const CxtPtSet empty;
    return empty;
  }
  SVFGNode *defNode = svfg->getNode(cxtVar.get_id());
  if (!defNode) {
    static const CxtPtSet empty;
    return empty;
  }
  CxtLocDPItem dpm(cxtVar, defNode);
  return findPT(dpm);
}

CxtPtSet ContextDDA::computeDDAPts(const llvm::Value *ptr) {
  SVFG *svfg = getSVFG();
  if (!svfg || !ptr || !ptr->getType()->isPointerTy())
    return CxtPtSet{};
  const Value *v = ptr->stripPointerCasts();
  SVFGNode *defNode = svfg->getValueNode(v);
  if (!defNode) {
    if (const Instruction *inst = llvm::dyn_cast<Instruction>(v))
      defNode = svfg->getDef(inst);
    if (!defNode)
      return CxtPtSet{};
  }
  ContextCond cxt;
  CxtVar cxtVar(cxt, defNode->getId());
  return computeDDAPts(cxtVar);
}

void ContextDDA::reCompute(const CxtLocDPItem &dpm) {
  const SVFGNode *node = dpm.getLoc();
  SVFG *svfg = getSVFG();
  if (!node || !svfg)
    return;
  
  // Match SVF DDAVFSolver::reCompute: handle indirect call edge updates
  // Similar to FlowDDA, but with context-sensitive handling
  
  // Re-compute for transitive closures (out-edges)
  const std::vector<SVFGEdge *> &edgeSet = node->getOutEdges();
  reComputeForEdges(dpm, edgeSet, false);
}

void ContextDDA::reComputeForEdges(const CxtLocDPItem &dpm,
                                    const std::vector<SVFGEdge *> &edgeSet,
                                    bool indirectCall) {
  for (SVFGEdge *edge : edgeSet) {
    SVFGNode *dst = edge->getDstNode();
    if (!dst)
      continue;
    auto locIt = locToDpmSetMap_.find(dst->getId());
    if (locIt == locToDpmSetMap_.end())
      continue;
    
    // Match SVF DDAVFSolver::reComputeForEdges logic
    for (const CxtLocDPItem &dstDpm : locIt->second) {
      if (!indirectCall && isIndirectEdge(edge) && !isa<LoadSVFGNode>(dst)) {
        // For indirect edges (except to Load nodes), only recompute if same obj
        if (dstDpm.getCurNodeID() == dpm.getCurNodeID()) {
          clearbkVisited(dstDpm);
          findPT(dstDpm);
        }
      } else {
        // For direct edges or indirect call edges, always recompute
        clearbkVisited(dstDpm);
        findPT(dstDpm);
      }
    }
  }
}

void ContextDDA::answerQueries() {
  if (!client_ || !flowDDA_ || !getSVFG()) return;
  client_->setSVFG(getSVFG());
  client_->collectCandidateQueries();
  for (const llvm::Value *ptr : client_->getCandidateQueries())
    (void)computeDDAPts(ptr);
  client_->performStat(flowDDA_);
}
