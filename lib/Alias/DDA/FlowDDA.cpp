//===- FlowDDA.cpp -- Flow-sensitive demand-driven analysis ---------------//
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

#include "Alias/DDA/FlowDDA.h"
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

FlowDDA::FlowDDA() {
  ddaStat_ = std::make_unique<DDAStat>(this);
  setDDAStat(ddaStat_.get());
}

FlowDDA::~FlowDDA() = default;

void FlowDDA::answerQueries() {
  if (client_) {
    client_->setSVFG(svfg_.get());
    if (module_)
      client_->setModule(module_);
    client_->answerQueries(this);
  }
}

bool FlowDDA::handleBKCondition(LocDPItem &dpm, SVFGEdge *edge) {
  if (client_)
    client_->handleStatement(edge->getSrcNode(), dpm.getCurNodeID());
  return true;
}

void FlowDDA::handleOutOfBudgetDpm(const LocDPItem &dpm) {
  const PtsSet conservativePts = getConservativeCPts(dpm);
  if (!conservativePts.empty())
    DDAVFSolver<uint32_t, std::unordered_set<uint32_t>, LocDPItem,
                 FlowDDA>::updateCachedPointsTo(dpm, conservativePts);
  DDAVFSolver<uint32_t, std::unordered_set<uint32_t>, LocDPItem,
               FlowDDA>::addOutOfBudgetDpm(dpm);
}

bool FlowDDA::run(Module &M) {
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

void FlowDDA::unionDDAPts(PtsSet &target, const PtsSet &source) {
  for (uint32_t id : source)
    target.insert(id);
}

bool FlowDDA::unionDDAPts(const LocDPItem &dpm, const PtsSet &pts) {
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

void FlowDDA::resetQueryLoadMaps() {
  dpmToLoadDpmMap_.clear();
  dpmToLoadCVarMap_.clear();
}

void FlowDDA::insertOutOfBudgetDpm(const LocDPItem &dpm) {
  outOfBudgetDpms_.insert(dpm);
}

bool FlowDDA::isOutOfBudgetDpm(const LocDPItem &dpm) const {
  return outOfBudgetDpms_.count(dpm) != 0;
}

const FlowDDA::PtsSet &FlowDDA::getEmptyCPtSetRef() const {
  static const PtsSet empty;
  return empty;
}

void FlowDDA::setDpmLocVar(LocDPItem &dpm, SVFGNode *src, uint32_t ptrNodeId) {
  dpm.setLocVar(src, ptrNodeId);
}

void FlowDDA::connectIndirectCallees(const LocDPItem &dpm, const PtsSet &funPts,
                                     std::vector<SVFGEdge *> &newEdges) {
  const auto &indCallSites = svfg_->getIndCallSites(dpm.getCurNodeID());
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
}

void FlowDDA::forEachObjId(const PtsSet &pts,
                            std::function<void(uint32_t)> callback) const {
  for (uint32_t id : pts)
    callback(id);
}

void FlowDDA::forEachElementInCPtSet(
    const PtsSet &pts,
    std::function<void(uint32_t, uint32_t)> callback) const {
  for (uint32_t id : pts)
    callback(id, id);
}

bool FlowDDA::propagateViaObj(uint32_t storeObj, uint32_t loadObj) const {
  return storeObj == loadObj;
}

void FlowDDA::resolveFunPtr(const LocDPItem &dpm) {
  const SVFGNode *node = dpm.getLoc();
  if (!node || !svfg_)
    return;
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
  if (const FormalParmSVFGNode *formalParm =
          dyn_cast<FormalParmSVFGNode>(node)) {
    const llvm::Function *fun = formalParm->getFunction();
    if (fun && !fun->isDeclaration()) {
      const auto &indCS = svfg_->getIndCallSitesInvokingCallee(fun);
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

void FlowDDA::addLoadDpmAndCVar(const LocDPItem &dpm,
                                  const LocDPItem &loadDpm,
                                  uint32_t loadCVarObjId) {
  auto it = dpmToLoadDpmMap_.find(dpm);
  if (it != dpmToLoadDpmMap_.end())
    it->second = loadDpm;
  else
    dpmToLoadDpmMap_.emplace(dpm, loadDpm);
  dpmToLoadCVarMap_[dpm] = loadCVarObjId;
}

bool FlowDDA::hasLoadDpm(const LocDPItem &dpm) const {
  return dpmToLoadDpmMap_.find(dpm) != dpmToLoadDpmMap_.end();
}

LocDPItem FlowDDA::getLoadDpm(const LocDPItem &dpm) const {
  auto it = dpmToLoadDpmMap_.find(dpm);
  if (it != dpmToLoadDpmMap_.end())
    return it->second;
  // SVF asserts here ("not found??"). Log a warning for debugging.
  llvm::errs() << "FlowDDA::getLoadDpm: loadDpm not found for dpm (cur="
               << dpm.getCurNodeID() << "); returning self as fallback\n";
  return dpm;
}

uint32_t FlowDDA::getLoadCVar(const LocDPItem &dpm) const {
  auto it = dpmToLoadCVarMap_.find(dpm);
  if (it != dpmToLoadCVarMap_.end())
    return it->second;
  // SVF asserts here ("not found??"). Log a warning for debugging.
  llvm::errs() << "FlowDDA::getLoadCVar: loadCVar not found for dpm (cur="
               << dpm.getCurNodeID() << "); returning curNodeID as fallback\n";
  return dpm.getCurNodeID();
}

bool FlowDDA::isMustAlias(const LocDPItem &loadDpm,
                            const LocDPItem &storeDpm) const {
  (void)loadDpm;
  (void)storeDpm;
  // Match upstream SVF DDAVFSolver default: FlowDDA does not implement must-alias.
  return false;
}

bool FlowDDA::isHeapCondMemObj(uint32_t objId,
                                 const StoreSVFGNode *store) const {
  (void)store;
  if (objId == 0 || !svfg_)
    return false;
  // Match upstream SVF FlowDDA: exclude heap/dummy objects from strong update.
  if (svfg_->isUnknownObject(objId))
    return true;
  return svfg_->isHeapObject(objId);
}

bool FlowDDA::isLocalCVarInRecursion(uint32_t objId) const {
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

bool FlowDDA::isArrayCondMemObj(uint32_t objId) const {
  if (!svfg_ || objId == 0)
    return false;
  return svfg_->isArrayObject(objId);
}

bool FlowDDA::isFieldInsenCondMemObj(uint32_t objId) const {
  if (!svfg_ || objId == 0)
    return false;
  return svfg_->isFieldInsensitiveObject(objId);
}

SVFGNode *FlowDDA::getDefNodeForValue(const Value *v) const {
  if (!svfg_ || !v)
    return nullptr;
  if (SVFGNode *n = svfg_->getValueNode(v))
    return n;
  if (const Instruction *inst = dyn_cast<Instruction>(v))
    return svfg_->getDef(inst);
  return nullptr;
}

LocDPItem FlowDDA::getDPImWithOldCond(const LocDPItem &oldDpm,
                                       uint32_t objId,
                                       const SVFGNode *loc) const {
  LocDPItem dpm(oldDpm);
  dpm.setLocVar(loc, objId);
  // Match SVF DDAVFSolver::getDPImWithOldCond: add load info for Store/Load nodes.
  FlowDDA *nonConstThis = const_cast<FlowDDA *>(this);
  if (isa<StoreSVFGNode>(loc) || isa<StoreChiSVFGNode>(loc))
    nonConstThis->addLoadDpmAndCVar(dpm, getLoadDpm(oldDpm), objId);
  if (isa<LoadSVFGNode>(loc) || isa<LoadMuSVFGNode>(loc))
    nonConstThis->addLoadDpmAndCVar(dpm, oldDpm, objId);
  return dpm;
}

bool FlowDDA::isDirectEdge(SVFGEdge *e) {
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

bool FlowDDA::isIndirectEdge(SVFGEdge *e) {
  if (!e)
    return false;
  const SVFGEdgeK k = e->getEdgeKind();
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

void FlowDDA::handleAddr(PtsSet &pts, const LocDPItem &,
                          const AddrSVFGNode *addr) {
  if (!addr)
    return;

  // Query PTA for the value's object IDs.  This returns IDs from both the
  // SVFG (ensureBaseObjIdForValue) and the pointer analysis, which is
  // required because indirect-edge guards are populated from PTA IDs.
  //
  // Note: AddrSVFGNode also carries an optional objectId_ field (mirrors
  // SVF's getPAGSrcNodeID) but it is only useful when the builder's object
  // IDs are guaranteed to match PTA edge guards.  We always prefer the
  // PTA-backed lookup to stay consistent with guard filtering.
  const Value *v = addr->getValue();
  if (!v)
    return;
  SVFGNodeBS objIds = getObjectIdsForValue(v);
  for (uint32_t id : objIds) {
    // SVF field-insensitivity check: if isFieldInsensitive(srcID) srcID = getFIObjVar(srcID)
    if (svfg_ && svfg_->isFieldInsensitiveObject(id) && svfgBuilder_) {
      uint32_t fiObj = svfgBuilder_->getOrCreateFIObjId(id);
      if (fiObj != 0) {
        pts.insert(fiObj);
        continue;
      }
    }
    pts.insert(id);
  }
}

FlowDDA::PtsSet FlowDDA::processGepPts(const GepSVFGNode *gep,
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

bool FlowDDA::isStrongUpdate(const PtsSet &dstPts,
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

FlowDDA::PtsSet FlowDDA::getPointsTo(const Value *ptr) {
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
  if (isOutOfBudget())
    handleOutOfBudgetDpm(dpm);
  result = getCachedPointsTo(dpm);
  return result;
}

FlowDDA::PtsSet FlowDDA::getPointsToCached(const Value *ptr) {
  auto it = ptsCache_.find(ptr);
  if (it != ptsCache_.end())
    return it->second;
  PtsSet result = getPointsTo(ptr);
  ptsCache_[ptr] = result;
  return result;
}

bool FlowDDA::getPointsToSet(const Value *ptr,
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

void FlowDDA::buildRecursionInfo() {
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
    if (scc.size() > 1) {
      // Multi-function SCC: all members are recursive.
      for (const llvm::Function *f : scc)
        recursiveFunctions_.insert(f);
    } else if (scc.size() == 1) {
      // Single-function SCC: check for self-recursion (self-edge in call graph).
      const llvm::Function *f = *scc.begin();
      auto it = callGraph.find(f);
      if (it != callGraph.end()) {
        for (const llvm::Function *callee : it->second) {
          if (callee == f) {
            recursiveFunctions_.insert(f);
            break;
          }
        }
      }
    }
  }
}

void FlowDDA::buildLoopInfo() {
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

bool FlowDDA::mayAlias(const Value *v1, const Value *v2) {
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

bool FlowDDA::mayNull(const Value *ptr) {
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

FlowDDA::PtsSet FlowDDA::getConservativeCPts(const LocDPItem &dpm) const {
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

SVFGNodeBS FlowDDA::getObjectIdsForValue(const Value *v) const {
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

bool FlowDDA::isRecursiveFunction(const Function *f) const {
  if (!f)
    return false;
  return recursiveFunctions_.count(f) != 0;
}

bool FlowDDA::isInLoop(const llvm::Instruction *inst) const {
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

bool FlowDDA::isTopLevelPtrStmt(const SVFGNode *stmt) const {
  // Match SVF DDAVFSolver::isTopLevelPtrStmt: Store and MRSVFG are not top-level
  if (!stmt)
    return false;
  return stmt->getNodeKind() != SVFGK::Store && !stmt->isMemNode();
}
