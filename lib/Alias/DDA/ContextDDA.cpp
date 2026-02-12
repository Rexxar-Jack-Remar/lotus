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
#include "IR/SVFG/SVFGStats.h"

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

static bool isIndirectEdgeImpl(SVFGEdge *e);

static bool isDirectEdgeImpl(SVFGEdge *e) {
  if (!e) return false;
  if (isIndirectEdgeImpl(e))
    return false;
  SVFGEdgeK k = e->getEdgeKind();
  return k == SVFGEdgeK::IntraCopy || k == SVFGEdgeK::IntraDirect ||
         k == SVFGEdgeK::IntraPhi || k == SVFGEdgeK::IntraGep ||
         k == SVFGEdgeK::CallDir || k == SVFGEdgeK::CallInd ||
         k == SVFGEdgeK::RetDir || k == SVFGEdgeK::RetInd ||
         k == SVFGEdgeK::ParamCall || k == SVFGEdgeK::ParamRet ||
         k == SVFGEdgeK::IntraCmp || k == SVFGEdgeK::IntraBranch;
}

static bool isIndirectEdgeImpl(SVFGEdge *e) {
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

bool ContextDDA::isDirectEdge(SVFGEdge *e) { return isDirectEdgeImpl(e); }
bool ContextDDA::isIndirectEdge(SVFGEdge *e) { return isIndirectEdgeImpl(e); }

ContextDDA::ContextDDA(FlowDDA *flowDDA, DDAClient *client)
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
  // Context-insensitive edge: skip context check (recursion or value-flow cycle).
  if (insensitveEdges_.count(edge))
    return true;
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

  const CxtPtSet conservativePts = getConservativeCPts(dpm);
  if (!conservativePts.empty())
    DDAVFSolver<CxtVar, CxtPtSet, CxtLocDPItem, ContextDDA>::updateCachedPointsTo(
        dpm, conservativePts);
  DDAVFSolver<CxtVar, CxtPtSet, CxtLocDPItem, ContextDDA>::addOutOfBudgetDpm(dpm);
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

  // Collect recursive SCCs: multi-function SCCs + self-recursive functions
  std::unordered_set<size_t> recursiveSccIds;
  for (size_t i = 0; i < sccs.size(); ++i) {
    if (sccs[i].size() > 1) {
      recursiveSccIds.insert(i);
    } else if (sccs[i].size() == 1) {
      // Check for self-recursion (self-edge in call graph)
      const llvm::Function *f = *sccs[i].begin();
      auto it = callGraph.find(f);
      if (it != callGraph.end()) {
        for (const llvm::Function *callee : it->second) {
          if (callee == f) {
            recursiveSccIds.insert(i);
            break;
          }
        }
      }
    }
  }

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
          // Both caller and callee must be in the same recursive SCC
          if (itCaller->second != itCallee->second)
            continue;
          if (recursiveSccIds.count(itCaller->second) == 0)
            continue;
          uint32_t csId = svfg ? svfg->getCallSiteId(cb, callee) : 0;
          if (csId != 0)
            recursiveCallSiteIds_.insert(csId);
        }
      }
  }
}

void ContextDDA::resetQueryLoadMaps() {
  dpmToLoadDpmMap_.clear();
  dpmToLoadCVarMap_.clear();
}

CxtPtSet ContextDDA::getConservativeCPts(const CxtLocDPItem &dpm) const {
  // Match SVF ContextDDA::getConservativeCPts: downgrade to FlowDDA
  if (!flowDDA_ || !getSVFG())
    return CxtPtSet{};
  
  const llvm::Value *v = getValueForCxtVar(dpm, getSVFG());
  if (!v || !v->getType()->isPointerTy())
    return CxtPtSet{};

  FlowDDA::PtsSet flowPts = flowDDA_->getPointsTo(v);
  CxtPtSet cxtPts;
  ContextCond cond;
  for (uint32_t objId : flowPts) {
    CxtVar var(cond, objId);
    cxtPts.insert(var);
  }
  return cxtPts;
}

void ContextDDA::setDpmLocVar(CxtLocDPItem &dpm, SVFGNode *src,
                               uint32_t ptrNodeId) {
  dpm.setLocVar(src, ptrNodeId);
}

void ContextDDA::addDDAPts(CxtPtSet &pts, uint32_t var) {
  pts.insert(CxtVar(ContextCond(), var));
}

void ContextDDA::unionDDAPts(CxtPtSet &target, const CxtPtSet &source) {
  for (const CxtVar &v : source)
    target.insert(v);
}

bool ContextDDA::unionDDAPts(const CxtLocDPItem &dpm, const CxtPtSet &pts) {
  const SVFGNode *loc = dpm.getLoc();
  if (!loc)
    return false;
  auto &cache = isTopLevelPtrStmt(loc) ? dpmToTLPtsMap_ : dpmToADPtsMap_;
  auto it = cache.find(dpm);
  const size_t oldSize = (it != cache.end()) ? it->second.size() : 0;
  if (it == cache.end())
    cache[dpm] = pts;
  else
    for (const CxtVar &v : pts)
      it->second.insert(v);
  it = cache.find(dpm);
  return (it != cache.end() && it->second.size() > oldSize);
}

const CxtPtSet &ContextDDA::getEmptyCPtSetRef() const {
  static const CxtPtSet empty;
  return empty;
}

bool ContextDDA::isTopLevelPtrStmt(const SVFGNode *stmt) const {
  if (!stmt)
    return false;
  return !llvm::isa<LoadSVFGNode>(stmt) && !llvm::isa<StoreSVFGNode>(stmt) &&
         !llvm::isa<LoadMuSVFGNode>(stmt) && !llvm::isa<StoreChiSVFGNode>(stmt);
}

void ContextDDA::connectIndirectCallees(const CxtLocDPItem &dpm,
                                       const CxtPtSet &funPts,
                                       std::vector<SVFGEdge *> &newEdges) {
  SVFG *svfg = getSVFG();
  if (!flowDDA_ || !flowDDA_->getSVFGBuilder() || !svfg)
    return;
  const auto &indCallSites = svfg->getIndCallSites(dpm.getCurNodeID());
  for (const llvm::CallBase *cs : indCallSites) {
    if (!cs)
      continue;
    for (const CxtVar &cv : funPts) {
      uint32_t objId = cv.get_id();
      if (objId == 0)
        continue;
      const llvm::Value *v = svfg->getObjectValue(objId);
      const llvm::Function *callee =
          llvm::dyn_cast_or_null<llvm::Function>(v);
      if (!callee || callee->isDeclaration())
        continue;
      (void)flowDDA_->getSVFGBuilder()->connectCallSiteToCalleeOnTheFly(
          svfg, cs, callee, newEdges);
    }
  }
}

void ContextDDA::insertOutOfBudgetDpm(const CxtLocDPItem &dpm) {
  outOfBudgetDpms_.insert(dpm);
}

bool ContextDDA::isOutOfBudgetDpm(const CxtLocDPItem &dpm) const {
  return outOfBudgetDpms_.count(dpm) != 0;
}

void ContextDDA::forEachObjId(const CxtPtSet &pts,
                              std::function<void(uint32_t)> callback) const {
  for (const CxtVar &v : pts)
    callback(v.get_id());
}

void ContextDDA::forEachElementInCPtSet(
    const CxtPtSet &pts,
    std::function<void(const CxtVar &, uint32_t)> callback) const {
  for (const CxtVar &v : pts)
    callback(v, v.get_id());
}

SVFGNodeBS ContextDDA::getObjectIdsForValue(const llvm::Value *v) const {
  return flowDDA_ ? flowDDA_->getObjectIdsForValue(v) : SVFGNodeBS{};
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
  // SVF asserts here ("not found??"). Log a warning for debugging.
  llvm::errs() << "ContextDDA::getLoadDpm: loadDpm not found for dpm (cur="
               << dpm.getCurNodeID() << "); returning self as fallback\n";
  return dpm;
}

uint32_t ContextDDA::getLoadCVar(const CxtLocDPItem &dpm) const {
  auto it = dpmToLoadCVarMap_.find(dpm);
  if (it != dpmToLoadCVarMap_.end())
    return it->second;
  // SVF asserts here ("not found??"). Log a warning for debugging.
  llvm::errs() << "ContextDDA::getLoadCVar: loadCVar not found for dpm (cur="
               << dpm.getCurNodeID() << "); returning curNodeID as fallback\n";
  return dpm.getCurNodeID();
}

bool ContextDDA::isMustAlias(const CxtLocDPItem &loadDpm,
                            const CxtLocDPItem &storeDpm) const {
  (void)loadDpm;
  (void)storeDpm;
  return false;
}

bool ContextDDA::isCondCompatible(const ContextCond &cxt1,
                                   const ContextCond &cxt2,
                                   bool singleton) const {
  // SVF: context conditions of local/global vars are compatible; singleton => true.
  if (singleton)
    return true;
  int i = static_cast<int>(cxt1.cxtSize()) - 1;
  int j = static_cast<int>(cxt2.cxtSize()) - 1;
  const CallStrCxt &ctx1 = cxt1.getContexts();
  const CallStrCxt &ctx2 = cxt2.getContexts();
  for (; i >= 0 && j >= 0; --i, --j) {
    if (ctx1[static_cast<size_t>(i)] != ctx2[static_cast<size_t>(j)])
      return false;
  }
  return true;
}

bool ContextDDA::propagateViaObj(const CxtVar &storeObj,
                                   const CxtLocDPItem &dpm,
                                   bool singleton) const {
  if (storeObj.get_id() != getLoadCVar(dpm))
    return false;
  return isCondCompatible(storeObj.get_cond(), dpm.getCond(), singleton);
}

void ContextDDA::initInsensitiveEdges() {
  insensitveEdges_.clear();
  SVFG *svfg = getSVFG();
  if (!svfg)
    return;
  SVFGStats stats(svfg);
  stats.performSCCAnalysis(SVFGStats::SVFGEdgeSet{});
  CxtLocDPItem dummy(CxtVar(ContextCond(), 0), nullptr);
  for (const auto &pair : *svfg) {
    SVFGNode *node = pair.second;
    if (!node)
      continue;
    for (SVFGEdge *edge : node->getInEdges()) {
      if (!edge)
        continue;
      if (!isCallEdge(edge) && !isRetEdge(edge))
        continue;
      uint32_t csId = 0;
      if (isCallEdge(edge))
        csId = getCSIDAtCall(dummy, edge);
      else
        csId = getCSIDAtRet(dummy, edge);
      if (csId != 0 && isEdgeInRecursion(csId)) {
        insensitveEdges_.insert(edge);
        continue;
      }
      if (stats.isEdgeInSVFGSCC(edge))
        insensitveEdges_.insert(edge);
    }
  }
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
  // call sites that may invoke this function using SVFG's callee index
  // (mirrors SVF's CallGraph::getIndCallSitesInvokingCallee).
  if (const FormalParmSVFGNode *formalParm =
          dyn_cast<FormalParmSVFGNode>(node)) {
    const llvm::Function *fun = formalParm->getFunction();
    if (fun && !fun->isDeclaration()) {
      const auto &indCS = svfg->getIndCallSitesInvokingCallee(fun);
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

  // Query PTA for the value's object IDs (same strategy as FlowDDA).
  const Value *v = addr->getValue();
  if (!v || !flowDDA_)
    return;
  SVFGBuilder *builder = flowDDA_->getSVFGBuilder();
  SVFG *svfg = getSVFG();
  SVFGNodeBS objIds = flowDDA_->getObjectIdsForValue(v);
  for (uint32_t id : objIds) {
    // SVF field-insensitivity check
    if (svfg && svfg->isFieldInsensitiveObject(id) && builder) {
      uint32_t fiObj = builder->getOrCreateFIObjId(id);
      if (fiObj != 0) {
        pts.insert(CxtVar(cond, fiObj));
        continue;
      }
    }
    pts.insert(CxtVar(cond, id));
  }
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
  if (isOutOfBudget()) {
    insertOutOfBudgetDpm(dpm);
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
