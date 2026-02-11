//===- DemandDrivenAA.cpp -- Demand-driven pointer analysis (SVF-style) --//
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
//===----------------------------------------------------------------------===//
//
// Implements value-flow-based demand-driven pointer analysis following
// SVF's FlowDDA / DDAVFSolver (FSE'16, TSE'18).
//
//===----------------------------------------------------------------------===//

#include "Alias/DDA/DemandDrivenAA.h"
#include "Alias/DDA/DDAClient.h"
#include "Alias/DDA/DDAStat.h"
#include "IR/SVFG/SVFGEdge.h"
#include "IR/SVFG/SVFGNode.h"

#include <llvm/Analysis/CaptureTracking.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/Casting.h>

#include <functional>
#include <queue>
#include <stack>
#include <unordered_set>
#include <vector>

using namespace llvm;
using namespace lotus::analysis;

uint32_t DPItem::maximumBudget = 100000u;

DemandDrivenAA::DemandDrivenAA() { ddaStat_ = std::make_unique<DDAStat>(this); }

DemandDrivenAA::~DemandDrivenAA() = default;

void DemandDrivenAA::answerQueries() {
  if (client_) {
    client_->setSVFG(svfg_.get());
    if (module_)
      client_->setModule(module_);
    client_->answerQueries(this);
  }
}

bool DemandDrivenAA::handleBKCondition(LocDPItem &dpm, SVFGEdge *edge) {
  if (client_)
    client_->handleStatement(edge->getSrcNode(), dpm.getCurNodeID());
  return true;
}

void DemandDrivenAA::handleOutOfBudgetDpm(const LocDPItem &dpm) {
  // Downgrade to conservative points-to (base PTA if available).
  const PtsSet conservativePts = getConservativeCPts(dpm);
  if (!conservativePts.empty())
    updateCachedPointsTo(dpm, conservativePts);
  addOutOfBudgetDpm(dpm);
}

bool DemandDrivenAA::run(Module &M) {
  if (initialized_)
    return true;
  try {
    icfg_ = std::make_unique<::ICFG>();
    icfgBuilder_ = std::make_unique<::ICFGBuilder>(icfg_.get());
    icfgBuilder_->build(&M);
    SVFGBuilderConfig cfg;
    // Match SVF FlowDDA/ContextDDA: indirect-call edges are inserted on-the-fly
    // when function-pointer points-to is discovered.
    cfg.resolveIndirectCalls = false;
    svfgBuilder_ = std::make_unique<SVFGBuilder>(cfg);
    SVFG *built = svfgBuilder_->build(icfg_.get());
    if (!built) {
      icfg_.reset();
      icfgBuilder_.reset();
      svfgBuilder_.reset();
      return false;
    }
    svfg_.reset(built);
    module_ = &M;
    buildRecursionInfo();
    buildLoopInfo();
  } catch (const std::exception &) {
    icfg_.reset();
    icfgBuilder_.reset();
    svfgBuilder_.reset();
    svfg_.reset();
    return false;
  }
  initialized_ = true;
  return true;
}

void DemandDrivenAA::resetQuery() {
  if (outOfBudget_)
    OOBResetVisited();
  locToDpmSetMap_.clear();
  dpmToLoadDpmMap_.clear();
  dpmToLoadCVarMap_.clear();
  numSteps_ = 0;
  outOfBudget_ = false;
  if (ddaStat_)
    ddaStat_->numOfStep = 0;
}

void DemandDrivenAA::clearbkVisited(const LocDPItem &dpm) {
  backwardVisited_.erase(dpm);
}

void DemandDrivenAA::markbkVisited(const LocDPItem &dpm) {
  backwardVisited_.insert(dpm);
}

bool DemandDrivenAA::isbkVisited(const LocDPItem &dpm) const {
  return backwardVisited_.count(dpm) != 0;
}

const DemandDrivenAA::PtsSet &
DemandDrivenAA::getCachedPointsTo(const LocDPItem &dpm) const {
  const auto &cache =
      isTopLevelPtrStmt(dpm.getLoc()) ? dpmToTLPtsMap_ : dpmToADPtsMap_;
  auto it = cache.find(dpm);
  static const PtsSet empty;
  return (it != cache.end()) ? it->second : empty;
}

bool DemandDrivenAA::unionDDAPts(const LocDPItem &dpm, const PtsSet &pts) {
  auto &cache =
      isTopLevelPtrStmt(dpm.getLoc()) ? dpmToTLPtsMap_ : dpmToADPtsMap_;
  auto it = cache.find(dpm);
  if (it == cache.end()) {
    cache[dpm] = pts;
    return !pts.empty();
  }
  size_t oldSize = it->second.size();
  for (uint32_t id : pts)
    it->second.insert(id);
  return it->second.size() != oldSize;
}

void DemandDrivenAA::unionDDAPts(PtsSet &target, const PtsSet &source) {
  // Match SVF DDAVFSolver::unionDDAPts(CPtSet&, const CPtSet&)
  // Simple union: insert all elements from source into target
  for (uint32_t id : source)
    target.insert(id);
}

void DemandDrivenAA::updateCachedPointsTo(const LocDPItem &dpm,
                                          const PtsSet &pts) {
  // Match SVF DDAVFSolver::updateCachedPointsTo: only recompute if pts grew
  if (unionDDAPts(dpm, pts)) {
    reCompute(dpm);
  }
}

void DemandDrivenAA::reCompute(const LocDPItem &dpm) {
  const SVFGNode *node = dpm.getLoc();
  if (!node || !svfg_)
    return;
  
  // Match SVF DDAVFSolver::reCompute: update call graph/SVFG for indirect calls
  // on-the-fly when function-pointer points-to changes.
  const auto &indCallSites = svfg_->getIndCallSites(dpm.getCurNodeID());
  if (!indCallSites.empty() && svfgBuilder_) {
    const PtsSet &funPts = getCachedPointsTo(dpm);
    std::vector<SVFGEdge *> newEdges;
    for (const CallBase *cs : indCallSites) {
      if (!cs)
        continue;
      for (uint32_t objId : funPts) {
        if (objId == 0)
          continue;
        const Value *v = svfg_->getObjectValue(objId);
        const Function *callee = dyn_cast_or_null<Function>(v);
        if (!callee || callee->isDeclaration())
          continue;
        (void)svfgBuilder_->connectCallSiteToCalleeOnTheFly(
            svfg_.get(), cs, callee, newEdges);
      }
    }
    if (!newEdges.empty()) {
      buildRecursionInfo();
      reComputeForEdges(dpm, newEdges, true);
    }
  }
  
  // Re-compute for transitive closures (out-edges)
  const std::vector<SVFGEdge *> &edgeSet = node->getOutEdges();
  reComputeForEdges(dpm, edgeSet, false);
}

void DemandDrivenAA::reComputeForEdges(const LocDPItem &dpm,
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
    for (const LocDPItem &dstDpm : locIt->second) {
      if (!indirectCall && isIndirectEdge(edge) && !isa<LoadSVFGNode>(dst)) {
        // For indirect edges (except to Load nodes), only recompute if same obj
        if (dstDpm.getCurNodeID() == dpm.getCurNodeID()) {
          if (ddaStat_)
            ddaStat_->numOfStepInCycle++;
          clearbkVisited(dstDpm);
          findPT(dstDpm);
        }
      } else {
        // For direct edges or indirect call edges, always recompute
        if (ddaStat_)
          ddaStat_->numOfStepInCycle++;
        clearbkVisited(dstDpm);
        findPT(dstDpm);
      }
    }
  }
}

void DemandDrivenAA::resolveFunPtr(const LocDPItem &dpm) {
  const SVFGNode *node = dpm.getLoc();
  if (!node || !svfg_)
    return;

  // SVF DDAVFSolver::resolveFunPtr case 1: at a call-site return node,
  // resolve the function pointer at this indirect call site.
  if (const ActualRetSVFGNode *retNode = dyn_cast<ActualRetSVFGNode>(node)) {
    const llvm::CallBase *cs = retNode->getCallSite();
    if (cs && !cs->getCalledFunction()) {
      const Value *calledOp = cs->getCalledOperand();
      if (calledOp && calledOp->getType()->isPointerTy()) {
        SVFGNode *funPtrNode = getDefNodeForValue(calledOp);
        if (funPtrNode) {
          LocDPItem funPtrDpm(funPtrNode->getId(), funPtrNode);
          findPT(funPtrDpm);
        }
      }
    }
  }

  // SVF DDAVFSolver::resolveFunPtr case 2: at a function entry node
  // (FormalParmSVFGNode), find indirect call sites that may invoke this
  // function and resolve their function pointers.
  // Uses the SVFG's funPtrToIndCallSites map instead of scanning the module.
  if (const FormalParmSVFGNode *formalParm =
          dyn_cast<FormalParmSVFGNode>(node)) {
    const llvm::Function *fun = formalParm->getFunction();
    if (fun && !fun->isDeclaration()) {
      // Look up indirect call sites whose function pointer could resolve to
      // this callee. We iterate all registered (funPtrNodeId -> callsite)
      // entries and resolve any whose call targets may include `fun`.
      for (auto &pair : *svfg_) {
        SVFGNode *n = pair.second;
        if (!n)
          continue;
        const auto &indCS = svfg_->getIndCallSites(n->getId());
        for (const llvm::CallBase *cs : indCS) {
          if (!cs || cs->getCalledFunction())
            continue;
          const Value *calledOp = cs->getCalledOperand();
          if (!calledOp || !calledOp->getType()->isPointerTy())
            continue;
          SVFGNode *funPtrNode = getDefNodeForValue(calledOp);
          if (funPtrNode) {
            LocDPItem funPtrDpm(funPtrNode->getId(), funPtrNode);
            findPT(funPtrDpm);
          }
        }
      }
    }
  }
}

void DemandDrivenAA::addLoadDpmAndCVar(const LocDPItem &dpm,
                                        const LocDPItem &loadDpm,
                                        uint32_t loadCVarObjId) {
  auto it = dpmToLoadDpmMap_.find(dpm);
  if (it != dpmToLoadDpmMap_.end())
    it->second = loadDpm;
  else
    dpmToLoadDpmMap_.emplace(dpm, loadDpm);
  dpmToLoadCVarMap_[dpm] = loadCVarObjId;
}

bool DemandDrivenAA::hasLoadDpm(const LocDPItem &dpm) const {
  return dpmToLoadDpmMap_.find(dpm) != dpmToLoadDpmMap_.end();
}

LocDPItem DemandDrivenAA::getLoadDpm(const LocDPItem &dpm) const {
  auto it = dpmToLoadDpmMap_.find(dpm);
  if (it != dpmToLoadDpmMap_.end())
    return it->second;
  return dpm;
}

uint32_t DemandDrivenAA::getLoadCVar(const LocDPItem &dpm) const {
  auto it = dpmToLoadCVarMap_.find(dpm);
  return (it != dpmToLoadCVarMap_.end()) ? it->second : dpm.getCurNodeID();
}

bool DemandDrivenAA::isMustAlias(const LocDPItem &loadDpm,
                                  const LocDPItem &storeDpm) const {
  (void)loadDpm;
  (void)storeDpm;
  // Match upstream SVF DDAVFSolver default: FlowDDA does not implement must-alias.
  return false;
}

bool DemandDrivenAA::propagateViaObj(uint32_t storeObjId,
                                      uint32_t loadCVarObjId) const {
  return storeObjId == loadCVarObjId;
}

bool DemandDrivenAA::isHeapCondMemObj(uint32_t objId,
                                       const StoreSVFGNode *store) const {
  (void)store;
  if (objId == 0 || !svfg_)
    return false;
  // Match upstream SVF FlowDDA: exclude heap/dummy objects from strong update.
  if (svfg_->isUnknownObject(objId))
    return true;
  return svfg_->isHeapObject(objId);
}

bool DemandDrivenAA::isLocalCVarInRecursion(uint32_t objId) const {
  if (!svfg_ || objId == 0)
    return false;
  const Value *v = svfg_->getObjectValue(objId);
  if (!v)
    return false;
  const llvm::Instruction *inst = dyn_cast<llvm::Instruction>(v);
  if (!inst)
    return false;
  const llvm::Function *f = inst->getFunction();
  return f && recursiveFunctions_.count(f) != 0;
}

bool DemandDrivenAA::isArrayCondMemObj(uint32_t objId) const {
  if (!svfg_ || objId == 0)
    return false;
  return svfg_->isArrayObject(objId);
}

bool DemandDrivenAA::isFieldInsenCondMemObj(uint32_t objId) const {
  if (!svfg_ || objId == 0)
    return false;
  return svfg_->isFieldInsensitiveObject(objId);
}

bool DemandDrivenAA::testOutOfBudget(const LocDPItem &dpm) {
  if (outOfBudget_)
    return true;
  if (ddaStat_)
    ddaStat_->numOfStep++;
  numSteps_++;
  if (numSteps_ > LocDPItem::getMaxBudget()) {
    outOfBudget_ = true;
    return true;
  }
  return isOutOfBudgetDpm(dpm);
}

void DemandDrivenAA::addOutOfBudgetDpm(const LocDPItem &dpm) {
  outOfBudgetDpms_.insert(dpm);
}

bool DemandDrivenAA::isOutOfBudgetDpm(const LocDPItem &dpm) const {
  return outOfBudgetDpms_.count(dpm) != 0;
}

void DemandDrivenAA::OOBResetVisited() {
  for (const auto &p : locToDpmSetMap_) {
    for (const LocDPItem &dpm : p.second) {
      if (!isOutOfBudgetDpm(dpm))
        clearbkVisited(dpm);
    }
  }
}

SVFGNode *DemandDrivenAA::getDefNodeForValue(const Value *v) const {
  if (!svfg_ || !v)
    return nullptr;
  if (SVFGNode *n = svfg_->getValueNode(v))
    return n;
  if (const Instruction *inst = dyn_cast<Instruction>(v))
    return svfg_->getDef(inst);
  return nullptr;
}

LocDPItem DemandDrivenAA::getDPImWithOldCond(const LocDPItem &oldDpm,
                                             uint32_t objId,
                                             const SVFGNode *loc) const {
  LocDPItem dpm(oldDpm);
  dpm.setLocVar(loc, objId);
  // Match SVF DDAVFSolver::getDPImWithOldCond: add load info for Store/Load nodes.
  // Note: This is non-const because it modifies member maps, but we need const for
  // the interface. We'll handle the const_cast internally.
  DemandDrivenAA *nonConstThis = const_cast<DemandDrivenAA *>(this);
  if (isa<StoreSVFGNode>(loc) || isa<StoreChiSVFGNode>(loc))
    nonConstThis->addLoadDpmAndCVar(dpm, getLoadDpm(oldDpm), objId);
  if (isa<LoadSVFGNode>(loc) || isa<LoadMuSVFGNode>(loc))
    nonConstThis->addLoadDpmAndCVar(dpm, oldDpm, objId);
  return dpm;
}

void DemandDrivenAA::addDpmToLoc(const LocDPItem &dpm) {
  const SVFGNode *loc = dpm.getLoc();
  if (loc)
    locToDpmSetMap_[loc->getId()].insert(dpm);
}

bool DemandDrivenAA::isDirectEdge(SVFGEdge *e) {
  if (!e)
    return false;
  if (isIndirectEdge(e))
    return false;
  const SVFGEdgeK k = e->getEdgeKind();
  return k == SVFGEdgeK::IntraCopy || k == SVFGEdgeK::IntraDirect ||
         k == SVFGEdgeK::IntraPhi || k == SVFGEdgeK::IntraGep ||
         k == SVFGEdgeK::CallDir || k == SVFGEdgeK::CallInd ||
         k == SVFGEdgeK::RetDir || k == SVFGEdgeK::RetInd ||
         k == SVFGEdgeK::ParamCall || k == SVFGEdgeK::ParamRet ||
         k == SVFGEdgeK::IntraCmp || k == SVFGEdgeK::IntraBranch;
}

bool DemandDrivenAA::isIndirectEdge(SVFGEdge *e) {
  if (!e)
    return false;
  const SVFGEdgeK k = e->getEdgeKind();
  // Note: SVF's "IndirectSVFGEdge" set is narrower than "all non-direct edges".
  // In particular, call/ret edges for indirect calls (CallInd/RetInd) are not
  // memory value-flow edges for DDA; DDA relies on ParamCall/ParamRet plus
  // memory edges (CallAIn/RetAOut/etc.) instead.
  if (k == SVFGEdgeK::IntraIndirect || k == SVFGEdgeK::ThreadMHPIndirectVF ||
      k == SVFGEdgeK::CallAIn || k == SVFGEdgeK::CallFIn ||
      k == SVFGEdgeK::RetAOut || k == SVFGEdgeK::RetFOut ||
      k == SVFGEdgeK::IntraMu || k == SVFGEdgeK::IntraChi ||
      k == SVFGEdgeK::CallMu || k == SVFGEdgeK::CallChi ||
      k == SVFGEdgeK::RetMu || k == SVFGEdgeK::EntryChi) {
    return true;
  }
  // Memory SSA builder uses IntraPhi/IntraCopy between memory nodes. Treat
  // those as indirect to match SVF's IndirectSVFGEdge semantics.
  if (k == SVFGEdgeK::IntraPhi || k == SVFGEdgeK::IntraCopy ||
      k == SVFGEdgeK::IntraDirect) {
    const SVFGNode *src = e->getSrcNode();
    const SVFGNode *dst = e->getDstNode();
    return (src && src->isMemNode()) || (dst && dst->isMemNode());
  }
  return false;
}

void DemandDrivenAA::handleAddr(PtsSet &pts, const LocDPItem &,
                                const AddrSVFGNode *addr) {
  if (!addr)
    return;
  const Value *v = addr->getValue();
  if (!v)
    return;
  SVFGNodeBS objIds = getObjectIdsForValue(v);
  for (uint32_t id : objIds)
    pts.insert(id);
}

void DemandDrivenAA::backwardPropDpm(PtsSet &pts, uint32_t ptrNodeId,
                                     const LocDPItem &oldDpm, SVFGEdge *edge) {
  SVFGNode *src = edge->getSrcNode();
  if (!src)
    return;
  LocDPItem dpm(oldDpm);
  dpm.setLocVar(src, ptrNodeId);
  
  // Match SVF DDAVFSolver::backwardPropDpm: handle context/path sensitivity
  if (!handleBKCondition(dpm, edge)) {
    if (ddaStat_)
      ddaStat_->numOfInfeasiblePath++;
    return;
  }
  
  // Match SVF: record load dpm/cvar when crossing indirect edge
  // SVF checks SVFUtil::isa<IndirectSVFGEdge>(edge), which matches our isIndirectEdge
  if (isIndirectEdge(edge))
    addLoadDpmAndCVar(dpm, getLoadDpm(oldDpm), getLoadCVar(oldDpm));
  
  if (ddaStat_)
    ddaStat_->numOfDPM++;
  
  // Match SVF: use unionDDAPts which may have special handling
  // SVF calls: unionDDAPts(pts, findPT(dpm))
  // This ensures proper union with potential special handling for top-level vs address-taken
  unionDDAPts(pts, findPT(dpm));
}

void DemandDrivenAA::backtraceAlongDirectVF(PtsSet &pts,
                                            const LocDPItem &oldDpm) {
  const SVFGNode *node = oldDpm.getLoc();
  if (!node || !svfg_)
    return;
  for (SVFGEdge *edge : node->getInEdges()) {
    if (!isDirectEdge(edge))
      continue;
    SVFGNode *src = edge->getSrcNode();
    SVFGNode *lhs = svfg_->getLHSTopLevPtr(src);
    if (lhs)
      backwardPropDpm(pts, lhs->getId(), oldDpm, edge);
  }
}

void DemandDrivenAA::backtraceAlongIndirectVF(PtsSet &pts,
                                              const LocDPItem &oldDpm,
                                              const PtsSet &curObjValues) {
  (void)curObjValues;
  const SVFGNode *node = oldDpm.getLoc();
  if (!node || !svfg_)
    return;
  const uint32_t obj = oldDpm.getCurNodeID();
  // Match SVF DDAVFSolver: skip constant objects (obj == 0 represents null/constant).
  if (obj == 0)
    return;
  for (SVFGEdge *edge : node->getInEdges()) {
    if (!isIndirectEdge(edge))
      continue;
    // Match SVF DDAVFSolver::backtraceAlongIndirectVF: only follow indirect edge
    // when obj is in guard (pointsTo set). In SVF: guard.test(obj) checks membership.
    const std::set<uint32_t> &guard = edge->getPointsTo();
    // Lotus SVFG may leave guard empty when PTA is unavailable/unknown; be conservative.
    if (!guard.empty() && guard.count(obj) == 0) {
      bool hasWildcard = false;
      for (uint32_t id : guard) {
        if (svfg_->isUnknownObject(id)) {
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

void DemandDrivenAA::startNewPTCompFromLoadSrc(PtsSet &loadPts,
                                              const LocDPItem &oldDpm) {
  const LoadSVFGNode *load = cast<LoadSVFGNode>(oldDpm.getLoc());
  uint32_t ptrNodeId = load->getLoadFromPtr();
  SVFGNode *loadSrc = svfg_->getNode(ptrNodeId);
  if (!loadSrc)
    return;
  SVFGEdge *edge =
      svfg_->getIntraVFGEdge(loadSrc, load, SVFGEdgeK::IntraDirect);
  if (edge)
    backwardPropDpm(loadPts, ptrNodeId, oldDpm, edge);
}

void DemandDrivenAA::startNewPTCompFromStoreDst(PtsSet &storePts,
                                               const LocDPItem &oldDpm) {
  const StoreSVFGNode *store = cast<StoreSVFGNode>(oldDpm.getLoc());
  uint32_t ptrNodeId = store->getStoreToPtr();
  SVFGNode *storeDst = svfg_->getNode(ptrNodeId);
  if (!storeDst)
    return;
  SVFGEdge *edge =
      svfg_->getIntraVFGEdge(storeDst, store, SVFGEdgeK::IntraDirect);
  if (edge)
    backwardPropDpm(storePts, ptrNodeId, oldDpm, edge);
}

void DemandDrivenAA::backtraceToStoreSrc(PtsSet &pts, const LocDPItem &oldDpm) {
  const StoreSVFGNode *store = cast<StoreSVFGNode>(oldDpm.getLoc());
  const Value *valueOperand =
      cast<StoreInst>(store->getValue())->getValueOperand();
  SVFGNode *storeSrc = getDefNodeForValue(valueOperand);
  if (!storeSrc)
    return;
  SVFGEdge *edge =
      svfg_->getIntraVFGEdge(storeSrc, store, SVFGEdgeK::IntraDirect);
  if (edge)
    backwardPropDpm(pts, storeSrc->getId(), oldDpm, edge);
}

DemandDrivenAA::PtsSet DemandDrivenAA::processGepPts(const GepSVFGNode *gep,
                                                     const PtsSet &srcPts) {
  if (!gep || !gep->getValue() || !isa<GetElementPtrInst>(gep->getValue()))
    return srcPts;
  
  const auto *gi = cast<GetElementPtrInst>(gep->getValue());
  PtsSet tmpDstPts;
  
  // Match SVF FlowDDA::processGepPts logic
  const bool isVariantFieldGep = !gi->hasAllConstantIndices();
  for (uint32_t objId : srcPts) {
    if (objId == 0) {
      tmpDstPts.insert(objId);
      continue;
    }
    uint32_t gepObjId = 0;
    if (svfgBuilder_) {
      gepObjId = svfgBuilder_->getGepObjectId(objId, gi);
      if (isVariantFieldGep && gepObjId == 0)
        gepObjId = svfgBuilder_->getOrCreateFIObjId(objId);
    }
    if (gepObjId == 0)
      gepObjId = objId;
    tmpDstPts.insert(gepObjId);
  }
  
  return tmpDstPts;
}

bool DemandDrivenAA::isStrongUpdate(const PtsSet &dstPts,
                                   const StoreSVFGNode *store) {
  if (dstPts.size() != 1)
    return false;
  const uint32_t objId = *dstPts.begin();
  // Match SVF DDAVFSolver::isStrongUpdate: exclude heap, array, field-insensitive, recursion
  if (isHeapCondMemObj(objId, store))
    return false;
  if (isArrayCondMemObj(objId))
    return false;
  if (isFieldInsenCondMemObj(objId))
    return false;
  if (isLocalCVarInRecursion(objId))
    return false;
  return true;
}

const DemandDrivenAA::PtsSet &DemandDrivenAA::findPT(const LocDPItem &dpm) {
  // Match SVF DDAVFSolver::findPT: check cache first
  if (isbkVisited(dpm)) {
    const PtsSet &cpts = getCachedPointsTo(dpm);
    return cpts;
  }

  // Mark as visited and add to location map
  markbkVisited(dpm);
  addDpmToLoc(dpm);

  // Match SVF DDAVFSolver::findPT: stop exploring when out-of-budget.
  if (testOutOfBudget(dpm) == false) {
    if (ddaStat_)
      ddaStat_->numOfDPM++;
    PtsSet pts;
    handleSingleStatement(dpm, pts);
    updateCachedPointsTo(dpm, pts);
  }
  return getCachedPointsTo(dpm);
}

void DemandDrivenAA::handleSingleStatement(const LocDPItem &dpm, PtsSet &pts) {
  const SVFGNode *node = dpm.getLoc();
  if (!node || !svfg_)
    return;
  resolveFunPtr(dpm);

  switch (node->getNodeKind()) {
  case SVFGK::Addr: {
    handleAddr(pts, dpm, cast<AddrSVFGNode>(node));
    break;
  }
  case SVFGK::Copy:
  case SVFGK::Phi:
  case SVFGK::IntraPhi:
  case SVFGK::InterPhi:
  case SVFGK::FormalParm:
  case SVFGK::ActualParm:
  case SVFGK::FormalRet:
  case SVFGK::ActualRet:
  case SVFGK::NullPtr: {
    backtraceAlongDirectVF(pts, dpm);
    break;
  }
  case SVFGK::Gep: {
    PtsSet gepPts;
    backtraceAlongDirectVF(gepPts, dpm);
    PtsSet filtered = processGepPts(cast<GepSVFGNode>(node), gepPts);
    pts.insert(filtered.begin(), filtered.end());
    break;
  }
  case SVFGK::Load: {
    const LoadSVFGNode *load = cast<LoadSVFGNode>(node);
    if (!load->getValue() || !load->getValue()->getType()->isPointerTy())
      break;
    PtsSet loadPts;
    startNewPTCompFromLoadSrc(loadPts, dpm);
    for (uint32_t objId : loadPts) {
      LocDPItem objDpm = getDPImWithOldCond(dpm, objId, load);
      // getDPImWithOldCond already adds loadDpm/loadCVar for Load nodes.
      backtraceAlongIndirectVF(pts, objDpm, PtsSet{});
    }
    break;
  }
  case SVFGK::Store: {
    const StoreSVFGNode *store = cast<StoreSVFGNode>(node);
    // SVF checks store->getPAGSrcNode()->isPointer(), i.e. whether the value
    // being stored is a pointer.  StoreInst::getType() is void, not the
    // stored value's type, so we must check the value operand explicitly.
    if (const StoreInst *si = dyn_cast_or_null<StoreInst>(store->getValue())) {
      if (!si->getValueOperand()->getType()->isPointerTy())
        break;
    } else {
      break;
    }
    
    // Match SVF DDAVFSolver Store handling: check must-alias first
    if (hasLoadDpm(dpm) && isMustAlias(getLoadDpm(dpm), dpm)) {
      if (ddaStat_)
        ddaStat_->numOfMustAliases++;
      backtraceToStoreSrc(pts, dpm);
      break;
    }
    
    // Get points-to set of store destination
    PtsSet storePts;
    startNewPTCompFromStoreDst(storePts, dpm);
    
    if (!storePts.empty()) {
      // Match SVF DDAVFSolver Store handling: for each store target, check propagateViaObj.
      bool hasStrongUpdate = false;
      for (uint32_t objId : storePts) {
        if (propagateViaObj(objId, getLoadCVar(dpm))) {
          LocDPItem objDpm = getDPImWithOldCond(dpm, objId, store);
          // getDPImWithOldCond already adds loadDpm/loadCVar for Store nodes.
          backtraceToStoreSrc(pts, objDpm);
          
          // Check strong update: if strong, only backtrace to src; else also indirect.
          if (isStrongUpdate(storePts, store)) {
            hasStrongUpdate = true;
            // Strong update: only backtrace to store source, no indirect backtrace
          } else {
            // Weak update: also do indirect backtrace
            backtraceAlongIndirectVF(pts, objDpm, PtsSet{});
          }
        } else {
          // When propagateViaObj is false, use original dpm (not objDpm).
          backtraceAlongIndirectVF(pts, dpm, PtsSet{});
        }
      }
      
      // Track strong update statistics (match SVF DDAVFSolver)
      if (hasStrongUpdate && ddaStat_) {
        ddaStat_->numOfStrongUpdates++;
        ddaStat_->strongUpdateStores.insert(store->getId());
      }
    }
    break;
  }
  default:
    if (node->isMemNode()) {
      backtraceAlongIndirectVF(pts, dpm, PtsSet{});
    }
    break;
  }
}

DemandDrivenAA::PtsSet DemandDrivenAA::getPointsTo(const Value *ptr) {
  PtsSet result;
  if (!initialized_ || !svfg_ || !ptr || !ptr->getType()->isPointerTy())
    return result;

  const Value *v = ptr->stripPointerCasts();
  SVFGNode *defNode = svfg_->getValueNode(v);
  if (!defNode) {
    if (const Instruction *inst = dyn_cast<Instruction>(v))
      defNode = svfg_->getDef(inst);
    if (!defNode)
      return result;
  }

  resetQuery();
  LocDPItem::setMaxBudget(kDefaultMaxBudget);
  LocDPItem dpm(defNode->getId(), defNode);
  (void)findPT(dpm);
  if (outOfBudget_)
    handleOutOfBudgetDpm(dpm);
  result = getCachedPointsTo(dpm);
  return result;
}

DemandDrivenAA::PtsSet DemandDrivenAA::getPointsToCached(const Value *ptr) {
  auto it = ptsCache_.find(ptr);
  if (it != ptsCache_.end())
    return it->second;
  PtsSet result = getPointsTo(ptr);
  ptsCache_[ptr] = result;
  return result;
}

bool DemandDrivenAA::getPointsToSet(const Value *ptr,
                                    std::vector<const Value *> &out) {
  out.clear();
  PtsSet pts = getPointsTo(ptr);
  if (!svfg_)
    return !pts.empty();
  for (uint32_t objId : pts) {
    if (const Value *v = svfg_->getObjectValue(objId))
      out.push_back(v);
  }
  return !out.empty();
}

void DemandDrivenAA::buildRecursionInfo() {
  recursiveFunctions_.clear();
  if (!module_)
    return;
  std::unordered_map<const llvm::Function *, std::vector<const llvm::Function *>>
      callGraph;
  for (const llvm::Function &F : *module_) {
    if (F.isDeclaration())
      continue;
    for (const llvm::BasicBlock &BB : F)
      for (const llvm::Instruction &I : BB) {
        const llvm::CallBase *cb = dyn_cast<llvm::CallBase>(&I);
        if (!cb)
          continue;
        std::vector<const llvm::Function *> callees;
        if (const llvm::Function *direct = cb->getCalledFunction()) {
          if (!direct->isDeclaration())
            callees.push_back(direct);
        } else if (svfg_) {
          const auto &connected = svfg_->getConnectedCallees(cb);
          for (const llvm::Function *callee : connected)
            callees.push_back(callee);
          if (callees.empty() && svfgBuilder_)
            callees = svfgBuilder_->getIndirectCallTargets(cb);
        } else if (svfgBuilder_) {
          callees = svfgBuilder_->getIndirectCallTargets(cb);
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
  std::unordered_map<const llvm::Function *, size_t> funcToScc;

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
      size_t id = sccs.size();
      sccs.push_back(std::move(scc));
      for (const llvm::Function *g : sccs[id])
        funcToScc[g] = id;
    }
  };

  for (const llvm::Function &F : *module_) {
    if (F.isDeclaration())
      continue;
    if (index.count(&F) == 0)
      strongConnect(&F);
  }

  for (const auto &scc : sccs) {
    if (scc.size() > 1)
      for (const llvm::Function *f : scc)
        recursiveFunctions_.insert(f);
  }
}

void DemandDrivenAA::buildLoopInfo() {
  loopInfoMap_.clear();
  if (!module_)
    return;
  for (const llvm::Function &F : *module_) {
    if (F.isDeclaration())
      continue;
    llvm::DominatorTree DT(const_cast<llvm::Function &>(F));
    auto LI = std::make_unique<llvm::LoopInfo>();
    LI->analyze(DT);
    loopInfoMap_[&F] = std::move(LI);
  }
}

bool DemandDrivenAA::mayAlias(const Value *v1, const Value *v2) {
  if (!initialized_ || !v1 || !v2)
    return true;
  const Value *p1 = v1->stripPointerCasts();
  const Value *p2 = v2->stripPointerCasts();
  if (!p1->getType()->isPointerTy() || !p2->getType()->isPointerTy())
    return false;
  if (p1 == p2)
    return true;
  PtsSet pts1 = getPointsToCached(p1);
  PtsSet pts2 = getPointsToCached(p2);
  for (uint32_t id : pts1) {
    if (pts2.count(id))
      return true;
  }
  return false;
}

bool DemandDrivenAA::mayNull(const Value *ptr) {
  if (!ptr || !ptr->getType()->isPointerTy())
    return false;
  PtsSet pts = getPointsToCached(ptr->stripPointerCasts());
  if (pts.count(0) != 0)
    return true;
  if (svfg_) {
    for (uint32_t id : pts) {
      if (svfg_->isUnknownObject(id))
        return true;
    }
  }
  return false;
}

DemandDrivenAA::PtsSet DemandDrivenAA::getConservativeCPts(const LocDPItem &dpm) const {
  if (!svfg_)
    return PtsSet{};
  const Value *v = nullptr;
  if (const Value *objVal = svfg_->getObjectValue(dpm.getCurNodeID()))
    v = objVal;
  else if (const SVFGNode *loc = dpm.getLoc())
    v = loc->getValue();
  if (!v || !v->getType()->isPointerTy())
    return PtsSet{};
  SVFGNodeBS ids = getObjectIdsForValue(v);
  PtsSet out;
  for (uint32_t id : ids)
    out.insert(id);
  return out;
}

SVFGNodeBS DemandDrivenAA::getObjectIdsForValue(const Value *v) const {
  SVFGNodeBS ids;
  if (!v || !v->getType()->isPointerTy())
    return ids;
  if (svfg_) {
    const uint32_t id = svfg_->getObjectId(v);
    if (id != 0)
      ids.insert(id);
  }
  if (svfgBuilder_) {
    SVFGNodeBS ptaIds = svfgBuilder_->getObjectIdsForValue(v);
    ids.insert(ptaIds.begin(), ptaIds.end());
  }
  return ids;
}

bool DemandDrivenAA::isRecursiveFunction(const Function *f) const {
  if (!f)
    return false;
  return recursiveFunctions_.count(f) != 0;
}

bool DemandDrivenAA::isInLoop(const llvm::Instruction *inst) const {
  if (!inst)
    return false;
  const llvm::Function *f = inst->getFunction();
  if (!f)
    return false;
  auto it = loopInfoMap_.find(f);
  if (it == loopInfoMap_.end() || !it->second)
    return false;
  const llvm::BasicBlock *bb = inst->getParent();
  return bb && it->second->getLoopFor(bb) != nullptr;
}

bool DemandDrivenAA::isTopLevelPtrStmt(const SVFGNode *stmt) const {
  // Match SVF DDAVFSolver::isTopLevelPtrStmt: Store and MRSVFG are not top-level
  if (!stmt)
    return false;
  return stmt->getNodeKind() != SVFGK::Store && !stmt->isMemNode();
}
