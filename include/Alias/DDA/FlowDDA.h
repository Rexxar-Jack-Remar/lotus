//===- FlowDDA.h -- Flow-sensitive demand-driven pointer analysis ---------//
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
//===----------------------------------------------------------------------===//
//
// FlowDDA: Value-flow-based demand-driven pointer analysis following
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
#include "Alias/DDA/DDAVFSolver.h"
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

/// Flow-sensitive, context-insensitive demand-driven pointer analysis using
/// the SVFG. Matches SVF's FlowDDA / DDAVFSolver algorithm.
class FlowDDA
    : public DDAVFSolver<uint32_t, std::unordered_set<uint32_t>, LocDPItem,
                         FlowDDA> {
  template <typename CVar, typename CPtSet, typename DPIm, typename D>
  friend class DDAVFSolver;

public:
  using PtsSet = std::unordered_set<uint32_t>;

  FlowDDA();
  virtual ~FlowDDA();

  FlowDDA(const FlowDDA &) = delete;
  FlowDDA &operator=(const FlowDDA &) = delete;

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

protected:
  // DDAVFSolver interface (CRTP). getSVFG/getSVFGBuilder/handleBKCondition are public above.
  SVFGNode *getDefNodeForValue(const llvm::Value *v) const;
  static bool isDirectEdge(SVFGEdge *e);
  static bool isIndirectEdge(SVFGEdge *e);
  PtsSet getConservativeCPts(const LocDPItem &dpm) const;
  void handleAddr(PtsSet &pts, const LocDPItem &dpm, const AddrSVFGNode *addr);
  PtsSet processGepPts(const GepSVFGNode *gep, const PtsSet &srcPts);
  bool isStrongUpdate(const PtsSet &dstPts, const StoreSVFGNode *store);
  uint32_t getPtrNodeID(uint32_t var) const { return var; }
  void addDDAPts(PtsSet &pts, uint32_t var) { pts.insert(var); }
  void unionDDAPts(PtsSet &target, const PtsSet &source);
  bool unionDDAPts(const LocDPItem &dpm, const PtsSet &pts);
  LocDPItem getDPImWithOldCond(const LocDPItem &oldDpm, uint32_t objId,
                               const SVFGNode *loc) const;
  void resolveFunPtr(const LocDPItem &dpm);
  bool isTopLevelPtrStmt(const SVFGNode *stmt) const;
  bool hasLoadDpm(const LocDPItem &dpm) const;
  LocDPItem getLoadDpm(const LocDPItem &dpm) const;
  uint32_t getLoadCVar(const LocDPItem &dpm) const;
  bool isMustAlias(const LocDPItem &loadDpm, const LocDPItem &storeDpm) const;
  bool propagateViaObj(uint32_t storeObj, uint32_t loadObj) const;
  void forEachObjId(const PtsSet &pts,
                    std::function<void(uint32_t)> callback) const;
  void forEachElementInCPtSet(
      const PtsSet &pts,
      std::function<void(uint32_t, uint32_t)> callback) const;
  const PtsSet &getEmptyCPtSetRef() const;
  void setDpmLocVar(LocDPItem &dpm, SVFGNode *src, uint32_t ptrNodeId);
  void addLoadDpmAndCVar(const LocDPItem &dpm, const LocDPItem &loadDpm,
                         uint32_t loadCVarObjId);
  void connectIndirectCallees(const LocDPItem &dpm, const PtsSet &funPts,
                              std::vector<SVFGEdge *> &newEdges);
  void onIndirectEdgesAdded() {
    // The SVFG has been mutated (new call edges added). Invalidate ptsCache_
    // so that subsequent mayAlias/mayNull calls recompute against the updated
    // graph rather than returning stale pre-mutation results (bug #9).
    ptsCache_.clear();
    buildRecursionInfo();
  }
  void resetQueryLoadMaps();
  void insertOutOfBudgetDpm(const LocDPItem &dpm);
  bool isOutOfBudgetDpm(const LocDPItem &dpm) const;
  uint32_t getMaxBudget() const { return LocDPItem::getMaxBudget(); }

private:
  /// Strong-update refinements: exclude heap, array, recursion (best-effort).
  bool isHeapCondMemObj(uint32_t objId, const StoreSVFGNode *store) const;
  bool isArrayCondMemObj(uint32_t objId) const;
  bool isFieldInsenCondMemObj(uint32_t objId) const;
  bool isLocalCVarInRecursion(uint32_t objId) const;

  PtsSet getPointsToCached(const llvm::Value *ptr);
  void buildRecursionInfo();
  void buildLoopInfo();

  std::unique_ptr<::ICFG> icfg_;
  std::unique_ptr<::ICFGBuilder> icfgBuilder_;
  std::unique_ptr<SVFGBuilder> svfgBuilder_;
  std::unique_ptr<SVFG> svfg_;

  std::unordered_map<const llvm::Value *, PtsSet> ptsCache_;
  std::map<LocDPItem, LocDPItem> dpmToLoadDpmMap_;
  std::map<LocDPItem, uint32_t> dpmToLoadCVarMap_;
  std::set<LocDPItem> outOfBudgetDpms_;
  bool initialized_ = false;
  DDAClient *client_ = nullptr;
  const llvm::Module *module_ = nullptr;
  std::unique_ptr<DDAStat> ddaStat_;
  std::unordered_set<const llvm::Function *> recursiveFunctions_;
  std::unordered_map<const llvm::Function *, std::unique_ptr<llvm::LoopInfo>>
      loopInfoMap_;
};

/// Backward-compatibility alias for code that still refers to DemandDrivenAA.
using DemandDrivenAA = FlowDDA;

} // namespace analysis
} // namespace lotus
