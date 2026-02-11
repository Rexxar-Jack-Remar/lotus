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

#include <llvm/Analysis/CaptureTracking.h>
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

static bool isIndirectEdge(SVFGEdge *e);

static bool isDirectEdge(SVFGEdge *e) {
  if (!e) return false;
  if (isIndirectEdge(e))
    return false;
  SVFGEdgeK k = e->getEdgeKind();
  return k == SVFGEdgeK::IntraCopy || k == SVFGEdgeK::IntraDirect ||
         k == SVFGEdgeK::IntraPhi || k == SVFGEdgeK::IntraGep ||
         k == SVFGEdgeK::CallDir || k == SVFGEdgeK::CallInd ||
         k == SVFGEdgeK::RetDir || k == SVFGEdgeK::RetInd ||
         k == SVFGEdgeK::ParamCall || k == SVFGEdgeK::ParamRet ||
         k == SVFGEdgeK::IntraCmp || k == SVFGEdgeK::IntraBranch;
}

static bool isIndirectEdge(SVFGEdge *e) {
  if (!e) return false;
  SVFGEdgeK k = e->getEdgeKind();
  if (k == SVFGEdgeK::IntraIndirect || k == SVFGEdgeK::ThreadMHPIndirectVF ||
      k == SVFGEdgeK::CallAIn || k == SVFGEdgeK::CallFIn ||
      k == SVFGEdgeK::RetAOut || k == SVFGEdgeK::RetFOut ||
      k == SVFGEdgeK::IntraMu || k == SVFGEdgeK::IntraChi ||
      k == SVFGEdgeK::CallMu || k == SVFGEdgeK::CallChi ||
      k == SVFGEdgeK::RetMu || k == SVFGEdgeK::EntryChi) {
    return true;
  }
  if (k == SVFGEdgeK::IntraPhi || k == SVFGEdgeK::IntraCopy ||
      k == SVFGEdgeK::IntraDirect) {
    const SVFGNode *src = e->getSrcNode();
    const SVFGNode *dst = e->getDstNode();
    return (src && src->isMemNode()) || (dst && dst->isMemNode());
  }
  return false;
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

uint32_t ContextDDA::getCSIDAtCall(CxtLocDPItem &, SVFGEdge *edge) {
  SVFG *svfg = getSVFG();
  if (!svfg || !edge)
    return 0;
  const llvm::CallBase *cs = edge->getCallSite();
  if (!cs)
    return 0;
  const llvm::Function *callee =
      edge->getDstNode() ? edge->getDstNode()->getFunction() : nullptr;
  return svfg->getCallSiteId(cs, callee);
}

uint32_t ContextDDA::getCSIDAtRet(CxtLocDPItem &, SVFGEdge *edge) {
  SVFG *svfg = getSVFG();
  if (!svfg || !edge)
    return 0;
  const llvm::CallBase *cs = edge->getCallSite();
  if (!cs)
    return 0;
  const llvm::Function *callee =
      edge->getSrcNode() ? edge->getSrcNode()->getFunction() : nullptr;
  return svfg->getCallSiteId(cs, callee);
}

bool ContextDDA::isEdgeInRecursion(uint32_t csId) const {
  return recursiveCallSiteIds_.count(csId) != 0;
}

void ContextDDA::popRecursiveCallSites(CxtLocDPItem &dpm) {
  // SVF: mark context as non-concrete since we lose precision crossing recursion.
  dpm.getCond().setNonConcreteCxt();
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
      if (isEdgeInRecursion(csId)) {
        // SVF: in recursion, just pop recursive call sites and skip matchContext.
        popRecursiveCallSites(dpm);
      } else {
        // Not in recursion: match call string. If mismatch, prune this path.
        if (!dpm.matchContext(csId))
          return false;
      }
    }
  } else if (isRetEdge(edge)) {
    uint32_t csId = getCSIDAtRet(dpm, edge);
    if (csId != 0) {
      if (isEdgeInRecursion(csId)) {
        // SVF: in recursion, just pop recursive call sites and skip pushContext.
        popRecursiveCallSites(dpm);
      } else {
        // SVF: if this call site ID is already in the call string, it may indicate
        // an undetected recursion. Mark as out-of-budget.
        if (dpm.getCond().containCallStr(csId)) {
          outOfBudget_ = true;
          return false;
        }
        // Push context for return edge (going backward = entering callee).
        dpm.pushContext(csId);
      }
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
  
  // Convert to context-insensitive points-to set (empty call-string)
  CxtPtSet cxtPts;
  ContextCond cond;
  for (uint32_t objId : flowPts) {
    CxtVar var(cond, objId);
    cxtPts.insert(var);
  }
  
  // Update cache with conservative points-to
  updateCachedPointsTo(dpm, cxtPts);
}

void ContextDDA::buildRecursionInfo() {
  recursiveCallSiteIds_.clear();
  const llvm::Module *module = flowDDA_ ? flowDDA_->getModule() : nullptr;
  SVFG *svfg = getSVFG();
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
        std::vector<const llvm::Function *> callees;
        if (const llvm::Function *direct = cb->getCalledFunction()) {
          if (!direct->isDeclaration())
            callees.push_back(direct);
        } else if (svfg) {
          const auto &connected = svfg->getConnectedCallees(cb);
          for (const llvm::Function *callee : connected)
            callees.push_back(callee);
        }
        for (const llvm::Function *callee : callees) {
          if (!callee || callee->isDeclaration())
            continue;
          callGraph[&F].push_back(callee);
        }
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
        std::vector<const llvm::Function *> callees;
        if (const llvm::Function *direct = cb->getCalledFunction()) {
          if (!direct->isDeclaration())
            callees.push_back(direct);
        } else if (svfg) {
          const auto &connected = svfg->getConnectedCallees(cb);
          for (const llvm::Function *callee : connected)
            callees.push_back(callee);
        }
        for (const llvm::Function *callee : callees) {
          if (!callee || callee->isDeclaration())
            continue;
          auto itCaller = funcToScc.find(&F);
          auto itCallee = funcToScc.find(callee);
          if (itCaller == funcToScc.end() || itCallee == funcToScc.end())
            continue;
          size_t sccId = itCaller->second;
          if (itCallee->second != sccId || sccs[sccId].size() <= 1)
            continue;
          uint32_t csId = svfg ? svfg->getCallSiteId(cb, callee) : 0;
          if (csId != 0)
            recursiveCallSiteIds_.insert(csId);
        }
      }
  }
}

void ContextDDA::resetQuery() {
  // Match SVF DDAVFSolver::resetQuery: clear OOB dpms if previous query was OOB.
  if (outOfBudget_) {
    for (const auto &p : locToDpmSetMap_) {
      for (const CxtLocDPItem &d : p.second) {
        if (outOfBudgetDpms_.count(d) == 0)
          backwardVisited_.erase(d);
      }
    }
  }
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
  // Match SVF DDAVFSolver::updateCachedPointsTo: recompute if pts grew.
  auto it = dpmToPtsMap_.find(dpm);
  if (it == dpmToPtsMap_.end()) {
    dpmToPtsMap_[dpm] = pts;
    // SVF calls reCompute whenever unionDDAPts returns true (non-empty new entry).
    if (!pts.empty())
      reCompute(dpm);
    return;
  }
  size_t oldSize = it->second.size();
  for (const CxtVar &v : pts)
    it->second.insert(v);
  if (it->second.size() != oldSize) {
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
  ContextCond cond;
  for (uint32_t objId : flowPts) {
    CxtVar var(cond, objId);
    cxtPts.insert(var);
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
  (void)loadDpm;
  (void)storeDpm;
  // Match upstream SVF DDAVFSolver default: ContextDDA does not implement must-alias.
  return false;
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

void ContextDDA::resolveFunPtr(const CxtLocDPItem &dpm) {
  SVFG *svfg = getSVFG();
  if (!svfg)
    return;
  const SVFGNode *node = dpm.getLoc();
  if (!node)
    return;

  // SVF case 1: at a call-site return node, resolve the function pointer.
  if (const ActualRetSVFGNode *retNode = dyn_cast<ActualRetSVFGNode>(node)) {
    const llvm::CallBase *cs = retNode->getCallSite();
    if (cs && !cs->getCalledFunction()) {
      const Value *calledOp = cs->getCalledOperand();
      if (calledOp && calledOp->getType()->isPointerTy()) {
        SVFGNode *funPtrNode = getDefNodeForValue(calledOp);
        if (funPtrNode) {
          // SVF ContextDDA preserves context when resolving function pointers.
          CxtVar funptrVar(dpm.getCondVar().get_cond(), funPtrNode->getId());
          CxtLocDPItem funPtrDpm(funptrVar, funPtrNode);
          findPT(funPtrDpm);
        }
      }
    }
  }

  // SVF case 2: at a function entry node (FormalParmSVFGNode), find indirect
  // call sites that may invoke this function using SVFG's indcall map.
  if (const FormalParmSVFGNode *formalParm =
          dyn_cast<FormalParmSVFGNode>(node)) {
    const llvm::Function *fun = formalParm->getFunction();
    if (fun && !fun->isDeclaration()) {
      for (auto &pair : *svfg) {
        SVFGNode *n = pair.second;
        if (!n)
          continue;
        const auto &indCS = svfg->getIndCallSites(n->getId());
        for (const llvm::CallBase *cs : indCS) {
          if (!cs || cs->getCalledFunction())
            continue;
          const Value *calledOp = cs->getCalledOperand();
          if (!calledOp || !calledOp->getType()->isPointerTy())
            continue;
          SVFGNode *funPtrNode = getDefNodeForValue(calledOp);
          if (funPtrNode) {
            CxtVar funptrVar(dpm.getCondVar().get_cond(),
                             funPtrNode->getId());
            CxtLocDPItem funPtrDpm(funptrVar, funPtrNode);
            findPT(funPtrDpm);
          }
        }
      }
    }
  }
}

CxtLocDPItem ContextDDA::getDPImWithOldCond(const CxtLocDPItem &oldDpm,
                                             uint32_t objId,
                                             const SVFGNode *loc) const {
  CxtVar var(oldDpm.getCond(), objId);
  CxtLocDPItem dpm(var, loc);
  // Match SVF DDAVFSolver::getDPImWithOldCond: add load info for Store/Load nodes.
  ContextDDA *nonConstThis = const_cast<ContextDDA *>(this);
  if (llvm::isa<StoreSVFGNode>(loc) || llvm::isa<StoreChiSVFGNode>(loc))
    nonConstThis->addLoadDpmAndCVar(dpm, getLoadDpm(oldDpm), objId);
  if (llvm::isa<LoadSVFGNode>(loc) || llvm::isa<LoadMuSVFGNode>(loc))
    nonConstThis->addLoadDpmAndCVar(dpm, oldDpm, objId);
  return dpm;
}

void ContextDDA::handleAddr(CxtPtSet &pts, const CxtLocDPItem &dpm,
                            const AddrSVFGNode *addr) {
  ContextCond cond = dpm.getCond();
  if (!addr)
    return;
  const Value *v = addr->getValue();
  if (!v || !flowDDA_)
    return;
  SVFGNodeBS objIds = flowDDA_->getObjectIdsForValue(v);
  for (uint32_t objId : objIds)
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
    // Lotus SVFG may leave guard empty when PTA is unavailable/unknown; be conservative.
    if (!guard.empty() && guard.count(obj) == 0) {
      bool hasWildcard = false;
      for (uint32_t id : guard) {
        if (svfg->isUnknownObject(id)) {
          hasWildcard = true;
          break;
        }
      }
      if (!hasWildcard)
        continue;
    }
    // SVF passes oldDpm.getCurNodeID() (the object being tracked) as ptrNodeId.
    // The object ID stays the same when backtracing along indirect (memory SSA) edges.
    backwardPropDpm(pts, oldDpm.getCurNodeID(), oldDpm, edge);
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
  const bool isVariantFieldGep = !gi->hasAllConstantIndices();
  SVFGBuilder *builder = flowDDA_ ? flowDDA_->getSVFGBuilder() : nullptr;
  for (const CxtVar &ptd : srcPts) {
    if (ptd.get_id() == 0) {
      tmpDstPts.insert(ptd);
      continue;
    }
    uint32_t newObjId = 0;
    if (builder) {
      newObjId = builder->getGepObjectId(ptd.get_id(), gi);
      if (isVariantFieldGep && newObjId == 0)
        newObjId = builder->getOrCreateFIObjId(ptd.get_id());
    }
    if (newObjId == 0)
      newObjId = ptd.get_id();
    tmpDstPts.insert(CxtVar(ptd.get_cond(), newObjId));
  }
  
  return tmpDstPts;
}

bool ContextDDA::isStrongUpdate(const CxtPtSet &dstPts,
                                const StoreSVFGNode *store) {
  if (dstPts.size() != 1)
    return false;
  (void)store;
  
  const CxtVar &var = *dstPts.begin();
  uint32_t objId = var.get_id();
  SVFG *svfg = getSVFG();
  if (!svfg)
    return false;
  // Match SVF ContextDDA::isStrongUpdate logic
  if (svfg->isUnknownObject(objId))
    return false;
  if (svfg->isHeapObject(objId)) {
    if (!var.get_cond().isConcreteCxt())
      return false;
    const Value *allocV = svfg->getObjectValue(objId);
    const Instruction *allocI = dyn_cast_or_null<Instruction>(allocV);
    const StoreInst *storeI =
        store ? dyn_cast_or_null<StoreInst>(store->getValue()) : nullptr;
    const Function *allocF = allocI ? allocI->getFunction() : nullptr;
    if (allocF && flowDDA_ && flowDDA_->isRecursiveFunction(allocF))
      return false;
    if (flowDDA_) {
      if (allocI && flowDDA_->isInLoop(allocI))
        return false;
      if (storeI && flowDDA_->isInLoop(storeI))
        return false;
    }
  }
  if (svfg->isArrayObject(objId))
    return false;
  if (svfg->isFieldInsensitiveObject(objId))
    return false;
  // Local variables in recursion
  if (svfg->isStackObject(objId)) {
    const Value *v = svfg->getObjectValue(objId);
    if (const Instruction *inst = dyn_cast_or_null<Instruction>(v)) {
      const Function *f = inst->getFunction();
      if (flowDDA_ && flowDDA_->isRecursiveFunction(f))
        return false;
    }
  }
  return true;
}

void ContextDDA::handleSingleStatement(const CxtLocDPItem &dpm, CxtPtSet &pts) {
  const SVFGNode *node = dpm.getLoc();
  SVFG *svfg = getSVFG();
  if (!node || !svfg) return;
  resolveFunPtr(dpm);
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
    // SVF checks store->getPAGSrcNode()->isPointer(): whether the value being
    // stored is a pointer.  StoreInst::getType() is void, so check the value
    // operand explicitly.
    if (const llvm::StoreInst *si =
            llvm::dyn_cast_or_null<llvm::StoreInst>(store->getValue())) {
      if (!si->getValueOperand()->getType()->isPointerTy())
        break;
    } else {
      break;
    }
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
  // Match SVF DDAVFSolver::findPT: check cache first
  if (isbkVisited(dpm)) {
    return getCachedPointsTo(dpm);
  }

  markbkVisited(dpm);
  if (dpm.getLoc())
    locToDpmSetMap_[dpm.getLoc()->getId()].insert(dpm);

  // Match SVF DDAVFSolver::findPT: stop exploring when out-of-budget.
  if (outOfBudgetDpms_.count(dpm))
    return getCachedPointsTo(dpm);
  if (outOfBudget_ || ++numSteps_ > kDefaultMaxBudget) {
    outOfBudget_ = true;
    return getCachedPointsTo(dpm);
  }

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
    if (const llvm::Value *v = svfg->getObjectValue(cxtVar.get_id()))
      defNode = getDefNodeForValue(v);
    if (!defNode) {
      static const CxtPtSet empty;
      return empty;
    }
  }
  CxtLocDPItem dpm(cxtVar, defNode);
  (void)findPT(dpm);
  if (outOfBudget_) {
    outOfBudgetDpms_.insert(dpm);
    handleOutOfBudgetDpm(dpm);
  }
  return getCachedPointsTo(dpm);
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
  
  // Match SVF ContextDDA::reCompute: refine indirect call edges on-the-fly.
  if (flowDDA_ && flowDDA_->getSVFGBuilder()) {
    const auto &indCallSites = svfg->getIndCallSites(dpm.getCurNodeID());
    if (!indCallSites.empty()) {
      const CxtPtSet &funPts = getCachedPointsTo(dpm);
      std::vector<SVFGEdge *> newEdges;
      for (const llvm::CallBase *cs : indCallSites) {
        if (!cs)
          continue;
        for (const CxtVar &cv : funPts) {
          const uint32_t objId = cv.get_id();
          if (objId == 0)
            continue;
          const llvm::Value *v = svfg->getObjectValue(objId);
          const llvm::Function *callee = llvm::dyn_cast_or_null<llvm::Function>(v);
          if (!callee || callee->isDeclaration())
            continue;
          (void)flowDDA_->getSVFGBuilder()->connectCallSiteToCalleeOnTheFly(
              svfg, cs, callee, newEdges);
        }
      }
      if (!newEdges.empty()) {
        buildRecursionInfo();
        reComputeForEdges(dpm, newEdges, true);
      }
    }
  }
  
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
  if (flowDDA_->getModule())
    client_->setModule(flowDDA_->getModule());
  client_->collectCandidateQueries();
  for (const llvm::Value *ptr : client_->getCandidateQueries())
    (void)computeDDAPts(ptr);
  client_->performStat(flowDDA_);
}
