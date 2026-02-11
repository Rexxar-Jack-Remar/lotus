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
  (void)dpm;
}

bool DemandDrivenAA::run(Module &M) {
  if (initialized_)
    return true;
  try {
    icfg_ = std::make_unique<::ICFG>();
    icfgBuilder_ = std::make_unique<::ICFGBuilder>(icfg_.get());
    icfgBuilder_->build(&M);
    SVFGBuilderConfig cfg;
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
  backwardVisited_.clear();
  dpmToTLPtsMap_.clear();
  dpmToADPtsMap_.clear();
  locToDpmSetMap_.clear();
  dpmToLoadDpmMap_.clear();
  dpmToLoadCVarMap_.clear();
  outOfBudgetDpms_.clear();
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
  for (const Value *v : pts)
    it->second.insert(v);
  return it->second.size() != oldSize;
}

void DemandDrivenAA::unionDDAPts(PtsSet &target, const PtsSet &source) {
  // Match SVF DDAVFSolver::unionDDAPts(CPtSet&, const CPtSet&)
  // Simple union: insert all elements from source into target
  for (const Value *v : source)
    target.insert(v);
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
  
  // Match SVF DDAVFSolver::reCompute: handle indirect call edge updates
  // Check if this is a function pointer node that may need indirect call resolution
  // In SVF, this checks _pag->isFunPtr(dpm.getCurNodeID())
  // We approximate by checking if the node's value is a function pointer type
  const Value *nodeVal = node->getValue();
  if (nodeVal && nodeVal->getType()->isPointerTy()) {
    // Check if it's a function pointer (used at indirect call sites)
    if (const Type *elemTy = nodeVal->getType()->getPointerElementType()) {
      if (isa<FunctionType>(elemTy)) {
        // This is a function pointer - may need to update indirect call edges
        // Note: In SVF, updateCallGraphAndSVFG is called here, but we don't
        // have direct call graph update capability. The SVFG builder handles
        // this during construction, so we skip explicit updates here.
      }
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
  
  // Match SVF DDAVFSolver::resolveFunPtr: handle call site return nodes
  // Check if this is a return node at a call site (ActualRetSVFGNode)
  if (const ActualRetSVFGNode *retNode = dyn_cast<ActualRetSVFGNode>(node)) {
    const llvm::CallBase *cs = retNode->getCallSite();
    if (cs && !cs->getCalledFunction()) {
      // Indirect call: resolve function pointer
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
  
  // Match SVF: handle function entry nodes (FormalRetSVFGNode at function entry)
  // When analyzing from a function entry, we need to resolve all indirect calls
  // that may call this function. This is less common in backward analysis but
  // may be needed for some forward-looking queries.
  if (const FormalRetSVFGNode *formalRet = dyn_cast<FormalRetSVFGNode>(node)) {
    const llvm::Function *fun = formalRet->getFunction();
    if (fun && !fun->isDeclaration() && module_) {
      // Find all indirect call sites that may call this function
      // We iterate through the module to find indirect calls
      for (const llvm::Function &F : *module_) {
        for (const llvm::BasicBlock &BB : F) {
          for (const llvm::Instruction &I : BB) {
            const llvm::CallBase *cb = dyn_cast<llvm::CallBase>(&I);
            if (!cb || cb->getCalledFunction())
              continue;
            // Indirect call: resolve function pointer
            const Value *calledOp = cb->getCalledOperand();
            if (calledOp && calledOp->getType()->isPointerTy()) {
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
  const PtsSet &loadPts = getCachedPointsTo(loadDpm);
  const PtsSet &storePts = getCachedPointsTo(storeDpm);
  return loadPts.size() == 1 && storePts.size() == 1 &&
         *loadPts.begin() == *storePts.begin();
}

bool DemandDrivenAA::propagateViaObj(uint32_t storeObjId,
                                      uint32_t loadCVarObjId) const {
  return storeObjId == loadCVarObjId;
}

bool DemandDrivenAA::isHeapCondMemObj(const Value *v,
                                       const StoreSVFGNode *store) const {
  if (!v)
    return false;
  
  // Match SVF FlowDDA::isHeapCondMemObj logic
  // SVF checks if the base object is HeapObjVar or DummyObjVar
  // We approximate by checking if v is a result of heap allocation calls
  
  // Check if v is a result of a heap allocation call
  if (const llvm::CallBase *cb = dyn_cast<llvm::CallBase>(v)) {
    const llvm::Function *callee = cb->getCalledFunction();
    if (callee) {
      llvm::StringRef name = callee->getName();
      // Standard heap allocation functions
      if (name == "malloc" || name == "calloc" || name == "realloc" ||
          name == "_Znam" || name == "_Znwm" || name == "aligned_alloc" ||
          name == "posix_memalign" || name == "memalign" || name == "valloc") {
        return true;
      }
    }
  }
  
  // Check if v is a result of operator new (C++)
  if (const llvm::Instruction *inst = dyn_cast<llvm::Instruction>(v)) {
    if (const llvm::CallBase *cb = dyn_cast<llvm::CallBase>(inst)) {
      const llvm::Function *callee = cb->getCalledFunction();
      if (callee) {
        llvm::StringRef name = callee->getName();
        // C++ new operators
        if (name.startswith("_Znw") || name.startswith("_Zna")) {
          return true;
        }
      }
    }
  }
  
  // In SVF, there are additional checks:
  // - Local allocated heap that hasn't escaped
  // - Not inside loop
  // - Not involved in recursion
  // These are more complex and require more context, so we use a conservative
  // approach: if it's a heap allocation, we mark it as heap
  
  return false;
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
  if (isa<StoreSVFGNode>(loc))
    nonConstThis->addLoadDpmAndCVar(dpm, getLoadDpm(oldDpm), objId);
  if (isa<LoadSVFGNode>(loc))
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
  SVFGEdgeK k = e->getEdgeKind();
  return k == SVFGEdgeK::IntraCopy || k == SVFGEdgeK::IntraDirect ||
         k == SVFGEdgeK::IntraPhi || k == SVFGEdgeK::IntraGep ||
         k == SVFGEdgeK::CallDir || k == SVFGEdgeK::RetDir ||
         k == SVFGEdgeK::ParamCall || k == SVFGEdgeK::ParamRet ||
         k == SVFGEdgeK::IntraCmp || k == SVFGEdgeK::IntraBranch;
}

bool DemandDrivenAA::isIndirectEdge(SVFGEdge *e) {
  if (!e)
    return false;
  SVFGEdgeK k = e->getEdgeKind();
  return k == SVFGEdgeK::IntraLoad || k == SVFGEdgeK::IntraStore ||
         k == SVFGEdgeK::IntraMu || k == SVFGEdgeK::IntraChi ||
         k == SVFGEdgeK::IntraIndirect || k == SVFGEdgeK::CallInd ||
         k == SVFGEdgeK::RetInd || k == SVFGEdgeK::ThreadMHPIndirectVF;
}

void DemandDrivenAA::handleAddr(PtsSet &pts, const LocDPItem &,
                                const AddrSVFGNode *addr) {
  if (const Value *v = addr->getValue())
    pts.insert(v);
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
  // In SVF, this checks _pag->isConstantObj(obj), which includes null and constants.
  if (obj == 0)
    return;
  for (SVFGEdge *edge : node->getInEdges()) {
    if (!isIndirectEdge(edge))
      continue;
    // Match SVF DDAVFSolver::backtraceAlongIndirectVF: only follow indirect edge
    // when obj is in guard (pointsTo set). In SVF: guard.test(obj) checks membership.
    const std::set<uint32_t> &guard = edge->getPointsTo();
    // If guard is empty, guard.test(obj) would return false, so don't follow.
    // If guard is not empty, only follow if obj is in guard.
    if (guard.empty() || guard.count(obj) == 0)
      continue;
    SVFGNode *src = edge->getSrcNode();
    if (!src)
      continue;
    SVFGNode *lhs = svfg_->getLHSTopLevPtr(src);
    if (!lhs)
      continue;
    backwardPropDpm(pts, lhs->getId(), oldDpm, edge);
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
  for (const Value *ptd : srcPts) {
    // Check if this is a block object or constant (pass through)
    // In SVF, isBlkObjOrConstantObj checks for null/undef/constant objects
    // We approximate by checking if it's a constant or null
    if (isa<Constant>(ptd) || isa<ConstantPointerNull>(ptd)) {
      tmpDstPts.insert(ptd);
      continue;
    }
    
    // Handle field-sensitive GEP
    // In SVF: if variant field GEP, mark field-insensitive; else getGepObjVar
    // For variant field GEPs (non-constant indices), SVF marks objects as field-insensitive
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
      // Variant field GEP: pass through the base object (field-insensitive)
      // In SVF, this would call setObjFieldInsensitive and getFIObjVar
      // For now, we pass through the base object
      tmpDstPts.insert(ptd);
    } else {
      // Constant field GEP: create field object
      // In SVF, this would call getGepObjVar with the field index
      // For now, we pass through the base object
      // TODO: Implement proper field-sensitive object creation when we have
      // object ID mapping and field tracking
      tmpDstPts.insert(ptd);
    }
  }
  
  return tmpDstPts;
}

bool DemandDrivenAA::isStrongUpdate(const PtsSet &dstPts,
                                   const StoreSVFGNode *store) {
  if (dstPts.size() != 1)
    return false;
  
  const Value *v = *dstPts.begin();
  
  // Match SVF DDAVFSolver::isStrongUpdate: exclude heap, array, field-insensitive, recursion
  if (isHeapCondMemObj(v, store))
    return false;
  
  // Check for array objects (arrays cannot have strong updates)
  if (const Type *elemTy = v->getType()->getPointerElementType()) {
    if (elemTy->isArrayTy())
      return false;
  }
  
  // Check for field-insensitive objects
  // In SVF, this checks isFieldInsenCondMemObj which checks baseObj->isFieldInsensitive()
  // We approximate by checking if the value is a struct pointer with variant GEPs
  // For now, we skip this check as we don't track field-insensitive flags
  
  // Check for local variables in recursion
  uint32_t objId = svfg_->getObjectId(v);
  if (objId != 0 && isLocalCVarInRecursion(objId))
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

  // Check out-of-budget
  if (testOutOfBudget(dpm)) {
    addOutOfBudgetDpm(dpm);
    handleOutOfBudgetDpm(dpm);
    // Return conservative points-to if available, otherwise empty
    PtsSet conservativePts = getConservativeCPts(dpm);
    if (!conservativePts.empty()) {
      if (isTopLevelPtrStmt(dpm.getLoc()))
        dpmToTLPtsMap_[dpm] = conservativePts;
      else
        dpmToADPtsMap_[dpm] = conservativePts;
    } else {
      if (isTopLevelPtrStmt(dpm.getLoc()))
        dpmToTLPtsMap_.emplace(dpm, PtsSet{});
      else
        dpmToADPtsMap_.emplace(dpm, PtsSet{});
    }
    return getCachedPointsTo(dpm);
  }

  if (ddaStat_)
    ddaStat_->numOfDPM++;
  
  // Compute points-to for this dpm
  PtsSet pts;
  handleSingleStatement(dpm, pts);
  
  // Update cache and recompute if needed
  updateCachedPointsTo(dpm, pts);
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
    for (const Value *v : loadPts) {
      uint32_t objId = svfg_->getObjectId(v);
      LocDPItem objDpm = getDPImWithOldCond(dpm, objId, load);
      // getDPImWithOldCond already adds loadDpm/loadCVar for Load nodes.
      backtraceAlongIndirectVF(pts, objDpm, PtsSet{});
    }
    break;
  }
  case SVFGK::Store: {
    const StoreSVFGNode *store = cast<StoreSVFGNode>(node);
    if (!store->getValue() || !store->getValue()->getType()->isPointerTy())
      break;
    
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
      for (const Value *v : storePts) {
        uint32_t objId = svfg_->getObjectId(v);
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
  const PtsSet &pts = findPT(dpm);
  result = pts;
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
  out.insert(out.end(), pts.begin(), pts.end());
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
  for (const Value *v : pts1) {
    if (pts2.count(v))
      return true;
  }
  return false;
}

bool DemandDrivenAA::mayNull(const Value *ptr) {
  if (!ptr || !ptr->getType()->isPointerTy())
    return false;
  return true;
}

DemandDrivenAA::PtsSet DemandDrivenAA::getConservativeCPts(const LocDPItem &dpm) const {
  // Default implementation: return empty set
  // Subclasses or clients can override to use base pointer analysis
  (void)dpm;
  return PtsSet{};
}

bool DemandDrivenAA::isTopLevelPtrStmt(const SVFGNode *stmt) const {
  // Match SVF DDAVFSolver::isTopLevelPtrStmt: Store and MRSVFG are not top-level
  if (!stmt)
    return false;
  return stmt->getNodeKind() != SVFGK::Store && !stmt->isMemNode();
}
