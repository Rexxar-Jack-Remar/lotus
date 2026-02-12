//===- DDAVFSolver.h -- Value-flow demand-driven solver (SVF-style) -------//
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
//===----------------------------------------------------------------------===//
//
// DDAVFSolver<Derived>: Shared value-flow backward solver for demand-driven
// pointer analysis. FlowDDA and ContextDDA instantiate this template with
// their CVar, CPtSet, and DPIm types (flow-sensitive only vs context-sensitive).
// Matches SVF's DDAVFSolver design (FSE'16, TSE'18).
//
//===----------------------------------------------------------------------===//

#pragma once

#include "Alias/DDA/DDAStat.h"
#include "IR/SVFG/SVFG.h"
#include "IR/SVFG/SVFGBase.h"
#include "IR/SVFG/SVFGEdge.h"
#include "IR/SVFG/SVFGNode.h"

#include <llvm/IR/Instructions.h>
#include <llvm/Support/Casting.h>

#include <functional>
#include <map>
#include <set>
#include <unordered_set>
#include <vector>

namespace lotus {
namespace analysis {

/// Value-flow backward solver template. Derived must define:
/// - CVar, CPtSet, DPIm (types)
/// - getSVFG(), getSVFGBuilder(), getDefNodeForValue(), getObjectIdsForValue()
/// - isDirectEdge(), isIndirectEdge(), handleBKCondition(), getConservativeCPts()
/// - handleAddr(), processGepPts(), isStrongUpdate(), getPtrNodeID(), addDDAPts()
/// - unionDDAPts(target, source), unionDDAPts(dpm, pts), getDPImWithOldCond()
/// - resolveFunPtr(), isTopLevelPtrStmt()
/// - hasLoadDpm(), getLoadDpm(), getLoadCVar(), isMustAlias(), propagateViaObj()
/// - forEachElementInCPtSet(), getEmptyCPtSetRef()
/// - setDpmLocVar(), addLoadDpmAndCVar(), connectIndirectCallees(), onIndirectEdgesAdded()
/// - insertOutOfBudgetDpm(), isOutOfBudgetDpm()
template <typename CVar, typename CPtSet, typename DPIm, typename Derived>
class DDAVFSolver {
public:

  DDAVFSolver() : ddaStat_(nullptr), numSteps_(0), outOfBudget_(false) {}
  virtual ~DDAVFSolver() = default;

  Derived &derived() { return *static_cast<Derived *>(this); }
  const Derived &derived() const {
    return *static_cast<const Derived *>(this);
  }

  void setDDAStat(DDAStat *s) { ddaStat_ = s; }
  DDAStat *getDDAStat() const { return ddaStat_; }

  /// Core: compute points-to for dpm; cache and return.
  const CPtSet &findPT(const DPIm &dpm) {
    if (isbkVisited(dpm))
      return getCachedPointsTo(dpm);
    markbkVisited(dpm);
    addDpmToLoc(dpm);
    if (!testOutOfBudget(dpm)) {
      if (ddaStat_)
        ddaStat_->numOfDPM++;
      CPtSet pts;
      handleSingleStatement(dpm, pts);
      updateCachedPointsTo(dpm, pts);
    }
    return getCachedPointsTo(dpm);
  }

  void resetQuery() {
    if (outOfBudget_)
      OOBResetVisited();
    locToDpmSetMap_.clear();
    derived().resetQueryLoadMaps();
    numSteps_ = 0;
    outOfBudget_ = false;
    if (ddaStat_)
      ddaStat_->numOfStep = 0;
  }

  bool isOutOfBudget() const { return outOfBudget_; }
  void setOutOfBudget(bool b) { outOfBudget_ = b; }

protected:
  void handleSingleStatement(const DPIm &dpm, CPtSet &pts) {
    const SVFGNode *node = dpm.getLoc();
    SVFG *svfg = derived().getSVFG();
    if (!node || !svfg)
      return;
    derived().resolveFunPtr(dpm);

    switch (node->getNodeKind()) {
    case SVFGK::Addr:
      derived().handleAddr(pts, dpm, llvm::cast<AddrSVFGNode>(node));
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
      CPtSet gepPts;
      backtraceAlongDirectVF(gepPts, dpm);
      CPtSet filtered = derived().processGepPts(llvm::cast<GepSVFGNode>(node),
                                                gepPts);
      derived().unionDDAPts(pts, filtered);
      break;
    }
    case SVFGK::Load: {
      const LoadSVFGNode *load = llvm::cast<LoadSVFGNode>(node);
      if (!load->getValue() ||
          !load->getValue()->getType()->isPointerTy())
        break;
      CPtSet loadPts;
      startNewPTCompFromLoadSrc(loadPts, dpm);
      derived().forEachElementInCPtSet(
          loadPts, [&](const CVar &obj, uint32_t /*objId*/) {
            DPIm objDpm = derived().getDPImWithOldCond(dpm, obj, load);
            backtraceAlongIndirectVF(pts, objDpm, CPtSet{});
          });
      break;
    }
    case SVFGK::Store: {
      const StoreSVFGNode *store = llvm::cast<StoreSVFGNode>(node);
      if (const llvm::StoreInst *si = llvm::dyn_cast_or_null<llvm::StoreInst>(
              store->getValue())) {
        if (!si->getValueOperand()->getType()->isPointerTy())
          break;
      } else {
        break;
      }
      if (derived().hasLoadDpm(dpm) &&
          derived().isMustAlias(derived().getLoadDpm(dpm), dpm)) {
        if (ddaStat_)
          ddaStat_->numOfMustAliases++;
        backtraceToStoreSrc(pts, dpm);
        break;
      }
      CPtSet storePts;
      startNewPTCompFromStoreDst(storePts, dpm);
      derived().forEachElementInCPtSet(
          storePts, [&](const CVar &storeObj, uint32_t /*objId*/) {
            if (derived().propagateViaObj(storeObj, derived().getLoadCVar(dpm))) {
              DPIm objDpm = derived().getDPImWithOldCond(dpm, storeObj, store);
              backtraceToStoreSrc(pts, objDpm);
              if (derived().isStrongUpdate(storePts, store)) {
                if (ddaStat_) {
                  ddaStat_->numOfStrongUpdates++;
                  ddaStat_->strongUpdateStores.insert(store->getId());
                }
              } else {
                backtraceAlongIndirectVF(pts, objDpm, CPtSet{});
              }
            } else {
              backtraceAlongIndirectVF(pts, dpm, CPtSet{});
            }
          });
      break;
    }
    default:
      if (node->isMemNode())
        backtraceAlongIndirectVF(pts, dpm, CPtSet{});
      break;
    }
  }

  void backtraceAlongDirectVF(CPtSet &pts, const DPIm &oldDpm) {
    const SVFGNode *node = oldDpm.getLoc();
    SVFG *svfg = derived().getSVFG();
    if (!node || !svfg)
      return;
    for (SVFGEdge *edge : node->getInEdges()) {
      if (!derived().isDirectEdge(edge))
        continue;
      SVFGNode *src = edge->getSrcNode();
      SVFGNode *lhs = svfg->getLHSTopLevPtr(src);
      if (lhs)
        backwardPropDpm(pts, lhs->getId(), oldDpm, edge);
    }
  }

  void backtraceAlongIndirectVF(CPtSet &pts, const DPIm &oldDpm,
                                const CPtSet &curObjPts) {
    (void)curObjPts;
    const SVFGNode *node = oldDpm.getLoc();
    SVFG *svfg = derived().getSVFG();
    if (!node || !svfg)
      return;
    uint32_t obj = oldDpm.getCurNodeID();
    if (obj == 0)
      return;
    for (SVFGEdge *edge : node->getInEdges()) {
      if (!derived().isIndirectEdge(edge))
        continue;
      const std::set<uint32_t> &guard = edge->getPointsTo();
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
      backwardPropDpm(pts, oldDpm.getCurNodeID(), oldDpm, edge);
    }
  }

  void backwardPropDpm(CPtSet &pts, uint32_t ptrNodeId, const DPIm &oldDpm,
                       SVFGEdge *edge) {
    SVFGNode *src = edge->getSrcNode();
    if (!src)
      return;
    DPIm dpm(oldDpm);
    derived().setDpmLocVar(dpm, src, ptrNodeId);
    if (!derived().handleBKCondition(dpm, edge)) {
      if (ddaStat_)
        ddaStat_->numOfInfeasiblePath++;
      return;
    }
    if (derived().isIndirectEdge(edge))
      derived().addLoadDpmAndCVar(dpm, derived().getLoadDpm(oldDpm),
                                   derived().getLoadCVar(oldDpm));
    if (ddaStat_)
      ddaStat_->numOfDPM++;
    derived().unionDDAPts(pts, findPT(dpm));
  }

  void startNewPTCompFromLoadSrc(CPtSet &loadPts, const DPIm &oldDpm) {
    const LoadSVFGNode *load = llvm::cast<LoadSVFGNode>(oldDpm.getLoc());
    SVFG *svfg = derived().getSVFG();
    if (!svfg)
      return;
    uint32_t ptrNodeId = load->getLoadFromPtr();
    SVFGNode *loadSrc = svfg->getNode(ptrNodeId);
    if (!loadSrc)
      return;
    SVFGEdge *edge =
        svfg->getIntraVFGEdge(loadSrc, load, SVFGEdgeK::IntraDirect);
    if (edge)
      backwardPropDpm(loadPts, ptrNodeId, oldDpm, edge);
  }

  void startNewPTCompFromStoreDst(CPtSet &storePts, const DPIm &oldDpm) {
    const StoreSVFGNode *store =
        llvm::cast<StoreSVFGNode>(oldDpm.getLoc());
    SVFG *svfg = derived().getSVFG();
    if (!svfg)
      return;
    uint32_t ptrNodeId = store->getStoreToPtr();
    SVFGNode *storeDst = svfg->getNode(ptrNodeId);
    if (!storeDst)
      return;
    SVFGEdge *edge =
        svfg->getIntraVFGEdge(storeDst, store, SVFGEdgeK::IntraDirect);
    if (edge)
      backwardPropDpm(storePts, ptrNodeId, oldDpm, edge);
  }

  void backtraceToStoreSrc(CPtSet &pts, const DPIm &oldDpm) {
    const StoreSVFGNode *store =
        llvm::cast<StoreSVFGNode>(oldDpm.getLoc());
    const llvm::Value *valueOperand =
        llvm::cast<llvm::StoreInst>(store->getValue())->getValueOperand();
    SVFGNode *storeSrc = derived().getDefNodeForValue(valueOperand);
    if (!storeSrc)
      return;
    SVFG *svfg = derived().getSVFG();
    if (!svfg)
      return;
	    SVFGEdge *edge =
	        svfg->getIntraVFGEdge(storeSrc, store, SVFGEdgeK::IntraDirect);
	    if (!edge)
	      return;
	    backwardPropDpm(pts, storeSrc->getId(), oldDpm, edge);
	  }

  void reCompute(const DPIm &dpm) {
    const SVFGNode *node = dpm.getLoc();
    SVFG *svfg = derived().getSVFG();
    if (!node || !svfg)
      return;
    const auto &indCallSites = svfg->getIndCallSites(dpm.getCurNodeID());
    if (!indCallSites.empty() && derived().getSVFGBuilder()) {
      const CPtSet &funPts = getCachedPointsTo(dpm);
      std::vector<SVFGEdge *> newEdges;
      derived().connectIndirectCallees(dpm, funPts, newEdges);
      if (!newEdges.empty()) {
        derived().onIndirectEdgesAdded();
        reComputeForEdges(dpm, newEdges, true);
      }
    }
    const std::vector<SVFGEdge *> &edgeSet = node->getOutEdges();
    reComputeForEdges(dpm, edgeSet, false);
  }

  void reComputeForEdges(const DPIm &dpm,
                         const std::vector<SVFGEdge *> &edgeSet,
                         bool indirectCall) {
    for (SVFGEdge *edge : edgeSet) {
      SVFGNode *dst = edge->getDstNode();
      if (!dst)
        continue;
      auto it = locToDpmSetMap_.find(dst->getId());
      if (it == locToDpmSetMap_.end())
        continue;
      for (const DPIm &dstDpm : it->second) {
        if (!indirectCall && derived().isIndirectEdge(edge) &&
            !llvm::isa<LoadSVFGNode>(dst)) {
          if (dstDpm.getCurNodeID() == dpm.getCurNodeID()) {
            if (ddaStat_)
              ddaStat_->numOfStepInCycle++;
            clearbkVisited(dstDpm);
            findPT(dstDpm);
          }
        } else {
          if (ddaStat_)
            ddaStat_->numOfStepInCycle++;
          clearbkVisited(dstDpm);
          findPT(dstDpm);
        }
      }
    }
  }

  void markbkVisited(const DPIm &dpm) { backwardVisited_.insert(dpm); }
  void clearbkVisited(const DPIm &dpm) { backwardVisited_.erase(dpm); }
  bool isbkVisited(const DPIm &dpm) const {
    return backwardVisited_.count(dpm) != 0;
  }

  const CPtSet &getCachedPointsTo(const DPIm &dpm) const {
    const auto &cache = derived().isTopLevelPtrStmt(dpm.getLoc())
                            ? dpmToTLPtsMap_
                            : dpmToADPtsMap_;
    auto it = cache.find(dpm);
    return (it != cache.end()) ? it->second : derived().getEmptyCPtSetRef();
  }

  void updateCachedPointsTo(const DPIm &dpm, const CPtSet &pts) {
    if (derived().unionDDAPts(dpm, pts))
      reCompute(dpm);
  }

  void addDpmToLoc(const DPIm &dpm) {
    const SVFGNode *loc = dpm.getLoc();
    if (loc)
      locToDpmSetMap_[loc->getId()].insert(dpm);
  }

  bool testOutOfBudget(const DPIm &dpm) {
    if (outOfBudget_)
      return true;
    if (ddaStat_)
      ddaStat_->numOfStep++;
    numSteps_++;
    if (numSteps_ > derived().getMaxBudget()) {
      outOfBudget_ = true;
      return true;
    }
    return derived().isOutOfBudgetDpm(dpm);
  }

  void addOutOfBudgetDpm(const DPIm &dpm) {
    derived().insertOutOfBudgetDpm(dpm);
  }

  void OOBResetVisited() {
    for (const auto &p : locToDpmSetMap_) {
      for (const DPIm &dpm : p.second) {
        if (!derived().isOutOfBudgetDpm(dpm))
          clearbkVisited(dpm);
      }
    }
  }

  std::set<DPIm> backwardVisited_;
  std::map<DPIm, CPtSet> dpmToTLPtsMap_;
  std::map<DPIm, CPtSet> dpmToADPtsMap_;
  std::map<uint32_t, std::set<DPIm>> locToDpmSetMap_;
  DDAStat *ddaStat_;
  uint32_t numSteps_;
  bool outOfBudget_;
};

} // namespace analysis
} // namespace lotus
