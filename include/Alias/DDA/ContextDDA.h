//===- ContextDDA.h -- Context-sensitive DDA (SVF-style) ------------------//
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
//===----------------------------------------------------------------------===//
//
// ContextDDA: flow-sensitive, context-sensitive demand-driven pointer analysis.
// Uses CxtLocDPItem (call-string context) and CxtPtSet. On out-of-budget
// downgrades to FlowDDA (DemandDrivenAA).
//
//===----------------------------------------------------------------------===//

#pragma once

#include "Alias/DDA/CxtDPItem.h"
#include "Alias/DDA/DDAClient.h"
#include "Alias/DDA/DemandDrivenAA.h"
#include "IR/SVFG/SVFG.h"
#include "IR/SVFG/SVFGEdge.h"
#include "IR/SVFG/SVFGNode.h"

#include <llvm/IR/Value.h>

#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace llvm {
class Module;
} // namespace llvm

namespace lotus {
namespace analysis {

/// Flow-sensitive, context-sensitive DDA. Uses call-string context in handleBKCondition.
class ContextDDA {
public:

  explicit ContextDDA(DemandDrivenAA *flowDDA, DDAClient *client);
  ~ContextDDA();

  bool run(llvm::Module &M);
  void setClient(DDAClient *client) { client_ = client; }
  DDAClient *getClient() const { return client_; }
  DemandDrivenAA *getFlowDDA() const { return flowDDA_; }
  SVFG *getSVFG() const { return flowDDA_ ? flowDDA_->getSVFG() : nullptr; }

  /// Compute context-sensitive points-to for pointer value (empty context).
  CxtPtSet computeDDAPts(const llvm::Value *ptr);
  /// Compute context-sensitive points-to for (context, node id).
  const CxtPtSet &computeDDAPts(const CxtVar &cxtVar);
  /// Run DDA for each candidate from the client.
  void answerQueries();

  /// Handle call-string on call/ret edges; return false to prune.
  bool handleBKCondition(CxtLocDPItem &dpm, SVFGEdge *edge);
  void handleOutOfBudgetDpm(const CxtLocDPItem &dpm);

  /// Call site ID from edge (from getCallSite()); 0 if none.
  uint32_t getCSIDAtCall(CxtLocDPItem &dpm, SVFGEdge *edge);
  uint32_t getCSIDAtRet(CxtLocDPItem &dpm, SVFGEdge *edge);

  /// True if call site csId is in a recursive SCC (Tarjan on module call graph).
  bool isEdgeInRecursion(uint32_t csId) const;
  /// Pop recursive call sites from dpm's context until top is not in recursion.
  void popRecursiveCallSites(CxtLocDPItem &dpm);

  static void setMaxCxtLen(uint32_t max) { ContextCond::setMaxCxtLen(max); }
  static void setMaxPathLen(uint32_t max) { ContextCond::setMaxPathLen(max); }

private:
  const CxtPtSet &findPT(const CxtLocDPItem &dpm);
  void handleSingleStatement(const CxtLocDPItem &dpm, CxtPtSet &pts);
  void handleAddr(CxtPtSet &pts, const CxtLocDPItem &dpm, const AddrSVFGNode *addr);
  void backtraceAlongDirectVF(CxtPtSet &pts, const CxtLocDPItem &oldDpm);
  void backtraceAlongIndirectVF(CxtPtSet &pts, const CxtLocDPItem &oldDpm,
                                const CxtPtSet &curObjPts);
  void backwardPropDpm(CxtPtSet &pts, uint32_t ptrNodeId,
                       const CxtLocDPItem &oldDpm, SVFGEdge *edge);
  void startNewPTCompFromLoadSrc(CxtPtSet &loadPts, const CxtLocDPItem &oldDpm);
  void startNewPTCompFromStoreDst(CxtPtSet &storePts, const CxtLocDPItem &oldDpm);
  void backtraceToStoreSrc(CxtPtSet &pts, const CxtLocDPItem &oldDpm);
  CxtPtSet processGepPts(const GepSVFGNode *gep, const CxtPtSet &srcPts);
  bool isStrongUpdate(const CxtPtSet &dstPts, const StoreSVFGNode *store);
  CxtLocDPItem getDPImWithOldCond(const CxtLocDPItem &oldDpm, uint32_t objId,
                                  const SVFGNode *loc) const;
  SVFGNode *getDefNodeForValue(const llvm::Value *v) const;
  uint32_t getOrCreateCSID(const llvm::CallBase *cs);
  void resetQuery();
  void buildRecursionInfo();
  void reCompute(const CxtLocDPItem &dpm);
  void reComputeForEdges(const CxtLocDPItem &dpm,
                         const std::vector<SVFGEdge *> &edgeSet,
                         bool indirectCall = false);

  void addLoadDpmAndCVar(const CxtLocDPItem &dpm, const CxtLocDPItem &loadDpm,
                         uint32_t loadCVarObjId);
  bool hasLoadDpm(const CxtLocDPItem &dpm) const;
  CxtLocDPItem getLoadDpm(const CxtLocDPItem &dpm) const;
  uint32_t getLoadCVar(const CxtLocDPItem &dpm) const;
  bool isMustAlias(const CxtLocDPItem &loadDpm, const CxtLocDPItem &storeDpm) const;
  bool propagateViaObj(uint32_t storeObjId, uint32_t loadCVarObjId) const;
  
  /// Visited flags for backward traversal
  void markbkVisited(const CxtLocDPItem &dpm);
  bool isbkVisited(const CxtLocDPItem &dpm) const;
  void clearbkVisited(const CxtLocDPItem &dpm);
  
  /// Points-to caching
  const CxtPtSet &getCachedPointsTo(const CxtLocDPItem &dpm) const;
  void updateCachedPointsTo(const CxtLocDPItem &dpm, const CxtPtSet &pts);
  
  /// Get conservative points-to from base pointer analysis (fallback when out-of-budget).
  CxtPtSet getConservativeCPts(const CxtLocDPItem &dpm) const;

  DemandDrivenAA *flowDDA_;
  DDAClient *client_;
  std::map<const llvm::CallBase *, uint32_t> callSiteToId_;
  uint32_t nextCallSiteId_ = 1;
  std::set<CxtLocDPItem> backwardVisited_;
  std::map<CxtLocDPItem, CxtPtSet> dpmToPtsMap_;
  std::map<uint32_t, std::set<CxtLocDPItem>> locToDpmSetMap_;
  std::map<CxtLocDPItem, CxtLocDPItem> dpmToLoadDpmMap_;
  std::map<CxtLocDPItem, uint32_t> dpmToLoadCVarMap_;
  uint32_t numSteps_ = 0;
  bool outOfBudget_ = false;
  static constexpr uint32_t kDefaultMaxBudget = 100000u;
  std::unordered_set<uint32_t> recursiveCallSiteIds_;
};

} // namespace analysis
} // namespace lotus
