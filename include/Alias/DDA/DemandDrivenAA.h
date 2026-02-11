//===- DemandDrivenAA.h -- Demand-driven pointer analysis (SVF-style) ---//
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
//===----------------------------------------------------------------------===//
//
// DemandDrivenAA: Value-flow-based demand-driven pointer analysis following
// SVF's FlowDDA / DDAVFSolver design (FSE'16, TSE'18).
//
// Mode: flow-sensitive, context-insensitive (SVF's FlowDDA). Context-sensitive
// mode (ContextDDA) is in ContextDDA.h/cpp. DDAClient (FunptrDDAClient,
// AliasDDAClient) supported via setClient/answerQueries.
//
// - findPT(dpm): backward compute points-to for (cur, loc); cache and reuse.
// - handleSingleStatement: Addr -> add allocation; Copy/Phi/param -> direct
//   backtrace; Gep -> direct + processGepPts; Load -> pts of pointer then
//   indirect backtrace; Store -> strong/weak update; MRSVFG -> indirect.
// - backtraceAlongDirectVF / backtraceAlongIndirectVF: follow in-edges by kind.
// - backwardPropDpm: dpm' = (ptr, pred); findPT(dpm') and union into pts.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "Alias/DDA/DPItem.h"
#include "IR/ICFG/ICFG.h"

namespace lotus {
namespace analysis {
class DDAClient;
class DDAStat;
} // namespace analysis
} // namespace lotus
#include "IR/ICFG/ICFGBuilder.h"
#include "IR/SVFG/SVFG.h"
#include "IR/SVFG/SVFGBase.h"
#include "IR/SVFG/SVFGBuilder.h"
#include "IR/SVFG/SVFGEdge.h"
#include "IR/SVFG/SVFGNode.h"

#include <llvm/IR/Value.h>

#include <map>
#include <memory>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace llvm {
class Module;
class Function;
class Instruction;
class LoopInfo;
} // namespace llvm

namespace lotus {
namespace analysis {

using LocDPItem = StmtDPItem<SVFGNode>;

/// Demand-driven pointer analysis using the SVFG (SVF-style algorithm).
class DemandDrivenAA {
public:
  using PtsSet = std::unordered_set<uint32_t>;

  DemandDrivenAA();
  ~DemandDrivenAA();

  DemandDrivenAA(const DemandDrivenAA &) = delete;
  DemandDrivenAA &operator=(const DemandDrivenAA &) = delete;

  bool run(llvm::Module &M);

  PtsSet getPointsTo(const llvm::Value *ptr);
  bool getPointsToSet(const llvm::Value *ptr,
                      std::vector<const llvm::Value *> &out);
  bool mayAlias(const llvm::Value *v1, const llvm::Value *v2);
  bool mayNull(const llvm::Value *ptr);

  SVFG *getSVFG() const { return svfg_.get(); }
  const SVFG *getSVFGConst() const { return svfg_.get(); }
  SVFGBuilder *getSVFGBuilder() const { return svfgBuilder_.get(); }
  const llvm::Module *getModule() const { return module_; }
  bool isInitialized() const { return initialized_; }

  /// Max steps per query (out-of-budget then fallback to base PTA).
  static constexpr uint32_t kDefaultMaxBudget = 100000u;

  /// Client for candidate queries and callbacks (SVF-style).
  void setClient(DDAClient *client) { client_ = client; }
  DDAClient *getClient() const { return client_; }
  /// Run DDA for each candidate from the client; no-op if no client.
  void answerQueries();
  /// FlowDDA: no context check; returns true. Override in ContextDDA.
  virtual bool handleBKCondition(LocDPItem &dpm, SVFGEdge *edge);
  /// Called when a dpm hits step budget (optional downgrade / stats).
  virtual void handleOutOfBudgetDpm(const LocDPItem &dpm);
  /// Entry used by client: compute points-to for pointer value (same as getPointsTo).
  PtsSet computeDDAPts(const llvm::Value *ptr) { return getPointsTo(ptr); }

  DDAStat *getStat() const { return ddaStat_.get(); }
  /// @brief Query object IDs for a pointer/allocation value (PTA-backed).
  SVFGNodeBS getObjectIdsForValue(const llvm::Value *v) const;
  /// @brief Query if a function is recursive in the current module.
  bool isRecursiveFunction(const llvm::Function *f) const;
  bool isInLoop(const llvm::Instruction *inst) const;

private:
  /// Union pts into cache for dpm; return true if cache grew. Used by updateCachedPointsTo.
  bool unionDDAPts(const LocDPItem &dpm, const PtsSet &pts);
  /// Union pts into target set (used by backwardPropDpm). Matches SVF's unionDDAPts(CPtSet&, const CPtSet&).
  void unionDDAPts(PtsSet &target, const PtsSet &source);
  /// Union new pts into cache; if grew, call reCompute(dpm).
  void updateCachedPointsTo(const LocDPItem &dpm, const PtsSet &pts);
  /// Re-run findPT on successors (out-edges) when cached pts grew.
  void reCompute(const LocDPItem &dpm);
  /// Re-compute for specific edge set (used by reCompute and for indirect calls).
  void reComputeForEdges(const LocDPItem &dpm,
                         const std::vector<SVFGEdge *> &edgeSet,
                         bool indirectCall = false);
  void clearbkVisited(const LocDPItem &dpm);
  void markbkVisited(const LocDPItem &dpm);
  bool isbkVisited(const LocDPItem &dpm) const;
  const PtsSet &getCachedPointsTo(const LocDPItem &dpm) const;

  /// Resolve function pointer at call/ret (ensure fun-ptr pts computed). No edge addition.
  void resolveFunPtr(const LocDPItem &dpm);

  /// Load/store must-alias and propagateViaObj (SVF-style).
  void addLoadDpmAndCVar(const LocDPItem &dpm, const LocDPItem &loadDpm,
                         uint32_t loadCVarObjId);
  bool hasLoadDpm(const LocDPItem &dpm) const;
  LocDPItem getLoadDpm(const LocDPItem &dpm) const;
  uint32_t getLoadCVar(const LocDPItem &dpm) const;
  virtual bool isMustAlias(const LocDPItem &loadDpm, const LocDPItem &storeDpm) const;
  bool propagateViaObj(uint32_t storeObjId, uint32_t loadCVarObjId) const;

  /// Strong-update refinements: exclude heap, array, recursion (best-effort).
  bool isHeapCondMemObj(uint32_t objId, const StoreSVFGNode *store) const;
  bool isArrayCondMemObj(uint32_t objId) const;
  bool isFieldInsenCondMemObj(uint32_t objId) const;
  bool isLocalCVarInRecursion(uint32_t objId) const;

  bool testOutOfBudget(const LocDPItem &dpm);
  void addOutOfBudgetDpm(const LocDPItem &dpm);
  bool isOutOfBudgetDpm(const LocDPItem &dpm) const;
  void OOBResetVisited();

  /// Core: compute points-to for dpm; cache and return.
  const PtsSet &findPT(const LocDPItem &dpm);

  /// Handle one node kind (Addr, Copy, Load, Store, Gep, Phi, param, MRSVFG).
  void handleSingleStatement(const LocDPItem &dpm, PtsSet &pts);

  void handleAddr(PtsSet &pts, const LocDPItem &dpm, const AddrSVFGNode *addr);
  void backtraceAlongDirectVF(PtsSet &pts, const LocDPItem &oldDpm);
  void backtraceAlongIndirectVF(PtsSet &pts, const LocDPItem &oldDpm,
                                const PtsSet &curObjValues);
  void backwardPropDpm(PtsSet &pts, uint32_t ptrNodeId, const LocDPItem &oldDpm,
                       SVFGEdge *edge);

  void startNewPTCompFromLoadSrc(PtsSet &loadPts, const LocDPItem &oldDpm);
  void startNewPTCompFromStoreDst(PtsSet &storePts, const LocDPItem &oldDpm);
  void backtraceToStoreSrc(PtsSet &pts, const LocDPItem &oldDpm);

  /// Gep: filter base pts by field (simplified: union all for now).
  PtsSet processGepPts(const GepSVFGNode *gep, const PtsSet &srcPts);

  /// Strong update when store dest is singleton and not heap/array.
  bool isStrongUpdate(const PtsSet &dstPts, const StoreSVFGNode *store);
  
  /// Get conservative points-to from base pointer analysis (fallback when out-of-budget).
  /// Default implementation returns empty set; can be overridden to use base PTA.
  virtual PtsSet getConservativeCPts(const LocDPItem &dpm) const;

  /// Create dpm with (objId, loc) for per-object backtrace (SVF getDPImWithOldCond).
  LocDPItem getDPImWithOldCond(const LocDPItem &oldDpm, uint32_t objId,
                               const SVFGNode *loc) const;

  /// Return SVFG node that defines \p v (value node or def of instruction).
  SVFGNode *getDefNodeForValue(const llvm::Value *v) const;

  /// Whether this is a top-level pointer statement (not Store or MRSVFG).
  bool isTopLevelPtrStmt(const SVFGNode *stmt) const;

  static bool isDirectEdge(SVFGEdge *e);
  static bool isIndirectEdge(SVFGEdge *e);

  void addDpmToLoc(const LocDPItem &dpm);

  PtsSet getPointsToCached(const llvm::Value *ptr);
  void resetQuery();
  void buildRecursionInfo();
  void buildLoopInfo();

  std::unique_ptr<::ICFG> icfg_;
  std::unique_ptr<::ICFGBuilder> icfgBuilder_;
  std::unique_ptr<SVFGBuilder> svfgBuilder_;
  std::unique_ptr<SVFG> svfg_;

  std::unordered_map<const llvm::Value *, PtsSet> ptsCache_;
  std::set<LocDPItem> backwardVisited_;
  std::map<LocDPItem, PtsSet> dpmToTLPtsMap_;
  std::map<LocDPItem, PtsSet> dpmToADPtsMap_;
  std::map<uint32_t, std::set<LocDPItem>> locToDpmSetMap_;
  std::map<LocDPItem, LocDPItem> dpmToLoadDpmMap_;
  std::map<LocDPItem, uint32_t> dpmToLoadCVarMap_;
  std::set<LocDPItem> outOfBudgetDpms_;
  uint32_t numSteps_ = 0;
  bool outOfBudget_ = false;
  bool initialized_ = false;
  DDAClient *client_ = nullptr;
  const llvm::Module *module_ = nullptr;
  std::unique_ptr<DDAStat> ddaStat_;
  std::unordered_set<const llvm::Function *> recursiveFunctions_;
  std::unordered_map<const llvm::Function *, std::unique_ptr<llvm::LoopInfo>>
      loopInfoMap_;
};

} // namespace analysis
} // namespace lotus
