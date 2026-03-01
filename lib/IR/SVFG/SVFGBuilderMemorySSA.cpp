//===- SVFGBuilderMemorySSA.cpp -- SVFG Memory SSA Implementation
//---------------------------//
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//
//===----------------------------------------------------------------------===//
//
// This file contains Memory SSA construction and interprocedural edge methods
//
//===----------------------------------------------------------------------===//

#include "IR/ICFG/ICFG.h"
#include "IR/SVFG/SVFGBuilder.h"

#include <queue>

#include <llvm/IR/Function.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Module.h>

using namespace lotus::analysis;
using namespace llvm;

// Helper function to get Module from ICFG
static const Module *getModuleFromICFG(const ICFG *icfg) {
  if (!icfg)
    return nullptr;

  // Iterate through ICFG nodes to find a function
  for (auto &pair : *icfg) {
    ICFGNode *node = pair.second;
    if (const Function *F = node->getFunction()) {
      return F->getParent();
    }
  }
  return nullptr;
}

static const ICFGNode *findICFGNodeForBlock(const ICFG *icfg,
                                            const BasicBlock *bb) {
  if (!icfg || !bb)
    return nullptr;
  return const_cast<ICFG *>(icfg)->getIntraBlockNode(bb);
}

static bool icfgHasCallEdgeTo(const ICFG *icfg, const CallBase *call,
                              const Function *callee) {
  if (!icfg || !call || !callee || callee->isDeclaration())
    return false;

  const ICFGNode *callerNode =
      const_cast<ICFG *>(icfg)->getIntraBlockNode(call->getParent());
  const ICFGNode *calleeEntry =
      const_cast<ICFG *>(icfg)->getIntraBlockNode(&callee->getEntryBlock());
  if (!callerNode || !calleeEntry)
    return false;

  for (const auto *edge : callerNode->getOutEdges()) {
    const auto *callEdge = llvm::dyn_cast<CallCFGEdge>(edge);
    if (!callEdge)
      continue;
    if (callEdge->getDstNode() == calleeEntry &&
        callEdge->getCallSite() == call)
      return true;
  }
  return false;
}

static std::vector<const Function *>
filterCalleesByICFG(const ICFG *icfg, const CallBase *call,
                    const std::vector<const Function *> &ptaCallees) {
  if (!icfg)
    return ptaCallees;

  std::vector<const Function *> filtered;
  filtered.reserve(ptaCallees.size());
  for (const Function *callee : ptaCallees) {
    if (icfgHasCallEdgeTo(icfg, call, callee)) {
      filtered.push_back(callee);
    }
  }

  // Keep PTA resolution if ICFG does not expose matching call edges.
  return filtered.empty() ? ptaCallees : filtered;
}

static SVFGNodeBS intersectPointsToSets(const SVFGNodeBS &lhs,
                                        const SVFGNodeBS &rhs,
                                        uint32_t unknownObjId) {
  // Empty means "no objects" (not "unknown"). Unknown is represented explicitly
  // via a wildcard object ID.
  if (lhs.empty() || rhs.empty())
    return SVFGNodeBS{};

  if (unknownObjId != 0 &&
      (lhs.count(unknownObjId) != 0 || rhs.count(unknownObjId) != 0)) {
    return SVFGNodeBS{unknownObjId};
  }

  SVFGNodeBS out;
  const SVFGNodeBS *small = &lhs;
  const SVFGNodeBS *large = &rhs;
  if (rhs.size() < lhs.size()) {
    small = &rhs;
    large = &lhs;
  }
  for (uint32_t id : *small) {
    if (large->count(id))
      out.insert(id);
  }
  return out;
}

void SVFGBuilder::buildMemorySSA() {
  if (!config.buildMSSA)
    return;

  const Module *M = getModuleFromICFG(icfg);
  if (!M)
    return;

  using MemRegPtsMap = std::unordered_map<uint32_t, SVFGNodeBS>;
  struct FunctionMemorySummary {
    std::unordered_set<unsigned> readArgs;
    std::unordered_set<unsigned> writeArgs;
    MemRegPtsMap readGlobals;
    MemRegPtsMap writeGlobals;
  };

  auto addRegion = [](MemRegPtsMap &dst, uint32_t memReg,
                      const SVFGNodeBS &pts) {
    auto &bucket = dst[memReg];
    bucket.insert(pts.begin(), pts.end());
  };

  auto getPtsForMemReg = [&](uint32_t memReg) -> SVFGNodeBS {
    if (memReg == 0)
      return {};
    auto objIt = memRegToObjId.find(memReg);
    if (objIt != memRegToObjId.end())
      return SVFGNodeBS{objIt->second};
    return SVFGNodeBS{getOrCreateUnknownObjId()};
  };

  auto mergeRegions = [&](MemRegPtsMap &dst, const MemRegPtsMap &src) {
    for (const auto &entry : src)
      addRegion(dst, entry.first, entry.second);
  };

  auto regionMapsEqual = [](const MemRegPtsMap &lhs, const MemRegPtsMap &rhs) {
    if (lhs.size() != rhs.size())
      return false;
    for (const auto &entry : lhs) {
      auto it = rhs.find(entry.first);
      if (it == rhs.end() || it->second != entry.second)
        return false;
    }
    return true;
  };

  auto summariesEqual = [&](const FunctionMemorySummary &lhs,
                            const FunctionMemorySummary &rhs) {
    return lhs.readArgs == rhs.readArgs && lhs.writeArgs == rhs.writeArgs &&
           regionMapsEqual(lhs.readGlobals, rhs.readGlobals) &&
           regionMapsEqual(lhs.writeGlobals, rhs.writeGlobals);
  };

  auto collectVisibleRegions = [&](const Value *ptr) -> MemRegPtsMap {
    MemRegPtsMap regions;
    if (!ptr || !ptr->getType()->isPointerTy())
      return regions;

    std::vector<const void *> ptsVoid = getPointsToSet(ptr);
    SVFGNodeBS objIds = convertPTAObjectsToObjIDs(ptsVoid);
    for (uint32_t objId : objIds) {
      if (!svfg->isGlobalObject(objId))
        continue;
      addRegion(regions, getOrCreateMemRegForObject(objId), SVFGNodeBS{objId});
    }

    if (!regions.empty())
      return regions;

    const Value *base = ptr->stripPointerCasts();
    if (isa<GlobalVariable>(base) || isa<GlobalAlias>(base)) {
      addRegion(regions, getOrCreateMemReg(base),
                SVFGNodeBS{getOrCreateUnknownObjId()});
    }

    return regions;
  };

  auto recordVisibleAccess = [&](const Function *context, const Value *ptr,
                                 FunctionMemorySummary &summary, bool isRead,
                                 bool isWrite) {
    if (!ptr || !ptr->getType()->isPointerTy())
      return;

    if (const auto *arg = dyn_cast<Argument>(ptr->stripPointerCasts())) {
      if (arg->getParent() == context) {
        if (isRead)
          summary.readArgs.insert(arg->getArgNo());
        if (isWrite)
          summary.writeArgs.insert(arg->getArgNo());
        return;
      }
    }

    MemRegPtsMap visibleRegs = collectVisibleRegions(ptr);
    if (isRead)
      mergeRegions(summary.readGlobals, visibleRegs);
    if (isWrite)
      mergeRegions(summary.writeGlobals, visibleRegs);
  };

  auto getCallTargets = [&](const CallBase *call) {
    std::vector<const Function *> callees;
    if (!call)
      return callees;

    if (const Function *directCallee = call->getCalledFunction()) {
      if (!directCallee->isDeclaration())
        callees.push_back(directCallee);
      return callees;
    }

    if (config.usePointerAnalysis && ptaSolverWrapper &&
        ptaSolverWrapper->solver && config.resolveIndirectCalls) {
      callees = getIndirectCallTargets(call);
      callees = filterCalleesByICFG(icfg, call, callees);
    }
    return callees;
  };

  auto collectDirectSummary = [&](const Function &F) {
    FunctionMemorySummary summary;
    for (const BasicBlock &bb : F) {
      for (const Instruction &inst : bb) {
        if (const auto *load = dyn_cast<LoadInst>(&inst)) {
          if (isAddressTakenPointer(load->getPointerOperand()))
            recordVisibleAccess(&F, load->getPointerOperand(), summary, true,
                                false);
          continue;
        }
        if (const auto *store = dyn_cast<StoreInst>(&inst)) {
          if (isAddressTakenPointer(store->getPointerOperand()))
            recordVisibleAccess(&F, store->getPointerOperand(), summary, false,
                                true);
          continue;
        }
        if (const auto *rmw = dyn_cast<AtomicRMWInst>(&inst)) {
          if (isAddressTakenPointer(rmw->getPointerOperand()))
            recordVisibleAccess(&F, rmw->getPointerOperand(), summary, true,
                                true);
          continue;
        }
        if (const auto *cmpxchg = dyn_cast<AtomicCmpXchgInst>(&inst)) {
          if (isAddressTakenPointer(cmpxchg->getPointerOperand()))
            recordVisibleAccess(&F, cmpxchg->getPointerOperand(), summary, true,
                                true);
          continue;
        }
        const auto *call = dyn_cast<CallBase>(&inst);
        if (!call)
          continue;
        const auto *intrinsic = dyn_cast<IntrinsicInst>(call);
        if (!intrinsic)
          continue;
        switch (intrinsic->getIntrinsicID()) {
        case Intrinsic::memcpy:
        case Intrinsic::memmove:
          if (call->arg_size() >= 1 &&
              call->getArgOperand(0)->getType()->isPointerTy()) {
            recordVisibleAccess(&F, call->getArgOperand(0), summary, false,
                                true);
          }
          if (call->arg_size() >= 2 &&
              call->getArgOperand(1)->getType()->isPointerTy()) {
            recordVisibleAccess(&F, call->getArgOperand(1), summary, true,
                                false);
          }
          break;
        case Intrinsic::memset:
          if (call->arg_size() >= 1 &&
              call->getArgOperand(0)->getType()->isPointerTy()) {
            recordVisibleAccess(&F, call->getArgOperand(0), summary, false,
                                true);
          }
          break;
        default:
          break;
        }
      }
    }
    return summary;
  };

  std::unordered_map<const Function *, FunctionMemorySummary> directSummaries;
  std::unordered_map<const Function *, FunctionMemorySummary> summaries;
  for (const Function &F : *M) {
    if (F.isDeclaration())
      continue;
    directSummaries[&F] = collectDirectSummary(F);
    summaries[&F] = directSummaries[&F];
  }

  bool summaryChanged = true;
  while (summaryChanged) {
    summaryChanged = false;
    for (const Function &F : *M) {
      if (F.isDeclaration())
        continue;

      FunctionMemorySummary next = directSummaries[&F];
      for (const BasicBlock &bb : F) {
        for (const Instruction &inst : bb) {
          const auto *call = dyn_cast<CallBase>(&inst);
          if (!call)
            continue;
          if (isa<IntrinsicInst>(call))
            continue;

          const bool mayRead = callMayReadMemory(call);
          const bool mayWrite = callMayModifyMemory(call);
          if (!mayRead && !mayWrite)
            continue;

          const std::vector<const Function *> callees = getCallTargets(call);
          if (callees.empty()) {
            for (unsigned i = 0; i < call->arg_size(); ++i) {
              const Value *arg = call->getArgOperand(i);
              if (!arg->getType()->isPointerTy())
                continue;
              recordVisibleAccess(&F, arg, next,
                                  mayRead && callArgMayReadMemory(call, i),
                                  mayWrite && callArgMayModifyMemory(call, i));
            }
            continue;
          }

          for (const Function *callee : callees) {
            auto calleeIt = summaries.find(callee);
            if (calleeIt == summaries.end())
              continue;
            const FunctionMemorySummary &calleeSummary = calleeIt->second;

            if (mayRead) {
              for (unsigned argIdx : calleeSummary.readArgs) {
                if (argIdx >= call->arg_size() ||
                    !callArgMayReadMemory(call, argIdx))
                  continue;
                recordVisibleAccess(&F, call->getArgOperand(argIdx), next, true,
                                    false);
              }
              mergeRegions(next.readGlobals, calleeSummary.readGlobals);
            }

            if (mayWrite) {
              for (unsigned argIdx : calleeSummary.writeArgs) {
                if (argIdx >= call->arg_size() ||
                    !callArgMayModifyMemory(call, argIdx))
                  continue;
                recordVisibleAccess(&F, call->getArgOperand(argIdx), next,
                                    false, true);
              }
              mergeRegions(next.writeGlobals, calleeSummary.writeGlobals);
            }
          }
        }
      }

      if (!summariesEqual(next, summaries[&F])) {
        summaries[&F] = std::move(next);
        summaryChanged = true;
      }
    }
  }

  std::unordered_map<const CallBase *, MemRegPtsMap> callReadRegions;
  std::unordered_map<const CallBase *, MemRegPtsMap> callWriteRegions;
  for (const Function &F : *M) {
    if (F.isDeclaration())
      continue;
    for (const BasicBlock &bb : F) {
      for (const Instruction &inst : bb) {
        const auto *call = dyn_cast<CallBase>(&inst);
        if (!call || isa<IntrinsicInst>(call))
          continue;

        const bool mayRead = callMayReadMemory(call);
        const bool mayWrite = callMayModifyMemory(call);
        if (!mayRead && !mayWrite)
          continue;

        MemRegPtsMap readRegs;
        MemRegPtsMap writeRegs;
        const std::vector<const Function *> callees = getCallTargets(call);
        if (callees.empty()) {
          for (unsigned i = 0; i < call->arg_size(); ++i) {
            const Value *arg = call->getArgOperand(i);
            if (!arg->getType()->isPointerTy())
              continue;
            MemRegPtsMap argRegs;
            std::vector<const void *> ptsVoid = getPointsToSet(arg);
            SVFGNodeBS argObjIds = convertPTAObjectsToObjIDs(ptsVoid);
            if (argObjIds.empty()) {
              addRegion(argRegs, getOrCreateMemReg(arg),
                        SVFGNodeBS{getOrCreateUnknownObjId()});
            } else {
              for (uint32_t objId : argObjIds) {
                addRegion(argRegs, getOrCreateMemRegForObject(objId),
                          SVFGNodeBS{objId});
              }
            }
            if (mayRead && callArgMayReadMemory(call, i))
              mergeRegions(readRegs, argRegs);
            if (mayWrite && callArgMayModifyMemory(call, i))
              mergeRegions(writeRegs, argRegs);
          }
        } else {
          for (const Function *callee : callees) {
            auto calleeIt = summaries.find(callee);
            if (calleeIt == summaries.end())
              continue;
            const FunctionMemorySummary &calleeSummary = calleeIt->second;

            if (mayRead) {
              for (unsigned argIdx : calleeSummary.readArgs) {
                if (argIdx >= call->arg_size() ||
                    !callArgMayReadMemory(call, argIdx))
                  continue;
                const Value *arg = call->getArgOperand(argIdx);
                if (!arg->getType()->isPointerTy())
                  continue;
                std::vector<const void *> ptsVoid = getPointsToSet(arg);
                SVFGNodeBS argObjIds = convertPTAObjectsToObjIDs(ptsVoid);
                if (argObjIds.empty()) {
                  addRegion(readRegs, getOrCreateMemReg(arg),
                            SVFGNodeBS{getOrCreateUnknownObjId()});
                } else {
                  for (uint32_t objId : argObjIds) {
                    addRegion(readRegs, getOrCreateMemRegForObject(objId),
                              SVFGNodeBS{objId});
                  }
                }
              }
              mergeRegions(readRegs, calleeSummary.readGlobals);
            }

            if (mayWrite) {
              for (unsigned argIdx : calleeSummary.writeArgs) {
                if (argIdx >= call->arg_size() ||
                    !callArgMayModifyMemory(call, argIdx))
                  continue;
                const Value *arg = call->getArgOperand(argIdx);
                if (!arg->getType()->isPointerTy())
                  continue;
                std::vector<const void *> ptsVoid = getPointsToSet(arg);
                SVFGNodeBS argObjIds = convertPTAObjectsToObjIDs(ptsVoid);
                if (argObjIds.empty()) {
                  addRegion(writeRegs, getOrCreateMemReg(arg),
                            SVFGNodeBS{getOrCreateUnknownObjId()});
                } else {
                  for (uint32_t objId : argObjIds) {
                    addRegion(writeRegs, getOrCreateMemRegForObject(objId),
                              SVFGNodeBS{objId});
                  }
                }
              }
              mergeRegions(writeRegs, calleeSummary.writeGlobals);
            }
          }
        }

        if (!readRegs.empty())
          callReadRegions.emplace(call, std::move(readRegs));
        if (!writeRegs.empty())
          callWriteRegions.emplace(call, std::move(writeRegs));
      }
    }
  }

  for (const Function &F : *M) {
    if (F.isDeclaration())
      continue;

    // First pass: create LoadMu and StoreChi nodes
    for (const BasicBlock &bb : F) {
      for (const Instruction &inst : bb) {
        if (const LoadInst *load = dyn_cast<LoadInst>(&inst)) {
          const Value *ptr = load->getPointerOperand();

          if (!isAddressTakenPointer(ptr))
            continue;

          std::vector<const void *> ptsVoid = getPointsToSet(ptr);
          // Convert PTA objects to abstract object IDs (NodeBS semantics).
          SVFGNodeBS objIds = convertPTAObjectsToObjIDs(ptsVoid);

          // Find ICFG node for this load
          const ICFGNode *icfgNode = nullptr;
          for (auto &pair : *icfg) {
            if (IntraBlockNode *blockNode =
                    dyn_cast<IntraBlockNode>(pair.second)) {
              if (blockNode->getBasicBlock() == &bb) {
                icfgNode = blockNode;
                break;
              }
            }
          }

          auto &muVec = loadToMuNodes[load];
          if (objIds.empty()) {
            // Unknown points-to: create one conservative MU for a value-keyed
            // region.
            const uint32_t memRegId = getOrCreateMemReg(ptr);
            const uint32_t muNodeId = nextNode();
            auto *muNode =
                new LoadMuSVFGNode(muNodeId, icfgNode, load, memRegId,
                                   SVFGNodeBS{getOrCreateUnknownObjId()});
            svfg->addNode(muNode);
            muVec.push_back(muNodeId);
          } else {
            for (uint32_t objId : objIds) {
              const uint32_t memRegId = getOrCreateMemRegForObject(objId);
              const uint32_t muNodeId = nextNode();
              SVFGNodeBS pts{objId};
              auto *muNode =
                  new LoadMuSVFGNode(muNodeId, icfgNode, load, memRegId, pts);
              svfg->addNode(muNode);
              muVec.push_back(muNodeId);
            }
          }
        }

        if (const StoreInst *store = dyn_cast<StoreInst>(&inst)) {
          const Value *ptr = store->getPointerOperand();

          if (!isAddressTakenPointer(ptr))
            continue;

          std::vector<const void *> ptsVoid = getPointsToSet(ptr);
          SVFGNodeBS objIds = convertPTAObjectsToObjIDs(ptsVoid);

          // Find ICFG node for this store
          const ICFGNode *icfgNode = nullptr;
          for (auto &pair : *icfg) {
            if (IntraBlockNode *blockNode =
                    dyn_cast<IntraBlockNode>(pair.second)) {
              if (blockNode->getBasicBlock() == &bb) {
                icfgNode = blockNode;
                break;
              }
            }
          }

          auto &chiVec = storeToChiNodes[store];
          if (objIds.empty()) {
            const uint32_t memRegId = getOrCreateMemReg(ptr);
            const uint32_t chiVersion = nextVersion(&F, memRegId);
            const uint32_t chiNodeId = nextNode();
            auto *chiNode = new StoreChiSVFGNode(
                chiNodeId, icfgNode, store, memRegId,
                SVFGNodeBS{getOrCreateUnknownObjId()}, chiVersion);
            svfg->addNode(chiNode);
            chiVec.push_back(chiNodeId);
            svfg->setMSSADef(memRegId, chiNode, chiVersion);

            // SVF MemorySSA invariant: Store statement defines its Chi version.
            // Needed so DDA can backtrace from StoreChi to StoreStmt.
            auto stIt = storeToStoreNode.find(store);
            if (stIt != storeToStoreNode.end()) {
              if (SVFGNode *storeStmt = svfg->getNode(stIt->second)) {
                SVFGNodeBS edgePts = chiNode->getDefSVFVars();
                if (edgePts.empty())
                  edgePts.insert(getOrCreateUnknownObjId());
                svfg->addEdge(storeStmt, chiNode, SVFGEdgeK::IntraChi, nullptr,
                              edgePts);
              }
            }
          } else {
            for (uint32_t objId : objIds) {
              const uint32_t memRegId = getOrCreateMemRegForObject(objId);
              const uint32_t chiVersion = nextVersion(&F, memRegId);
              const uint32_t chiNodeId = nextNode();
              SVFGNodeBS pts{objId};
              auto *chiNode = new StoreChiSVFGNode(chiNodeId, icfgNode, store,
                                                   memRegId, pts, chiVersion);
              svfg->addNode(chiNode);
              chiVec.push_back(chiNodeId);
              svfg->setMSSADef(memRegId, chiNode, chiVersion);

              auto stIt = storeToStoreNode.find(store);
              if (stIt != storeToStoreNode.end()) {
                if (SVFGNode *storeStmt = svfg->getNode(stIt->second)) {
                  SVFGNodeBS edgePts = chiNode->getDefSVFVars();
                  if (edgePts.empty())
                    edgePts.insert(getOrCreateUnknownObjId());
                  svfg->addEdge(storeStmt, chiNode, SVFGEdgeK::IntraChi,
                                nullptr, edgePts);
                }
              }
            }
          }
        }

        // Atomic memory ops behave as both load (Mu) and store (Chi).
        if (const auto *rmw = dyn_cast<AtomicRMWInst>(&inst)) {
          const Value *ptr = rmw->getPointerOperand();
          if (isAddressTakenPointer(ptr)) {
            std::vector<const void *> ptsVoid = getPointsToSet(ptr);
            SVFGNodeBS objIds = convertPTAObjectsToObjIDs(ptsVoid);
            const ICFGNode *icfgNode = findICFGNodeForBlock(icfg, &bb);

            auto &muVec = atomicToMuNodes[&inst];
            auto &chiVec = atomicToChiNodes[&inst];
            if (objIds.empty()) {
              const uint32_t memRegId = getOrCreateMemReg(ptr);
              const uint32_t muNodeId = nextNode();
              auto *muNode =
                  new LoadMuSVFGNode(muNodeId, icfgNode, nullptr, memRegId,
                                     SVFGNodeBS{getOrCreateUnknownObjId()});
              svfg->addNode(muNode);
              muVec.push_back(muNodeId);

              const uint32_t chiNodeId = nextNode();
              const uint32_t chiVersion = nextVersion(&F, memRegId);
              auto *chiNode = new StoreChiSVFGNode(
                  chiNodeId, icfgNode, nullptr, memRegId,
                  SVFGNodeBS{getOrCreateUnknownObjId()}, chiVersion);
              svfg->addNode(chiNode);
              chiVec.push_back(chiNodeId);
              svfg->setMSSADef(memRegId, chiNode, chiVersion);
            } else {
              for (uint32_t objId : objIds) {
                const uint32_t memRegId = getOrCreateMemRegForObject(objId);
                SVFGNodeBS pts{objId};

                const uint32_t muNodeId = nextNode();
                auto *muNode = new LoadMuSVFGNode(muNodeId, icfgNode, nullptr,
                                                  memRegId, pts);
                svfg->addNode(muNode);
                muVec.push_back(muNodeId);

                const uint32_t chiNodeId = nextNode();
                const uint32_t chiVersion = nextVersion(&F, memRegId);
                auto *chiNode = new StoreChiSVFGNode(
                    chiNodeId, icfgNode, nullptr, memRegId, pts, chiVersion);
                svfg->addNode(chiNode);
                chiVec.push_back(chiNodeId);
                svfg->setMSSADef(memRegId, chiNode, chiVersion);
              }
            }
          }
        }
        if (const auto *cmpxchg = dyn_cast<AtomicCmpXchgInst>(&inst)) {
          const Value *ptr = cmpxchg->getPointerOperand();
          if (isAddressTakenPointer(ptr)) {
            std::vector<const void *> ptsVoid = getPointsToSet(ptr);
            SVFGNodeBS objIds = convertPTAObjectsToObjIDs(ptsVoid);
            const ICFGNode *icfgNode = findICFGNodeForBlock(icfg, &bb);

            auto &muVec = atomicToMuNodes[&inst];
            auto &chiVec = atomicToChiNodes[&inst];
            if (objIds.empty()) {
              // Bug #6 fix: use unknownObjId (not empty SVFGNodeBS{}) so the
              // Mu/Chi nodes have a proper wildcard guard, consistent with the
              // AtomicRMWInst path and the LoadInst path.
              const uint32_t memRegId = getOrCreateMemReg(ptr);
              const uint32_t muNodeId = nextNode();
              auto *muNode =
                  new LoadMuSVFGNode(muNodeId, icfgNode, nullptr, memRegId,
                                     SVFGNodeBS{getOrCreateUnknownObjId()});
              svfg->addNode(muNode);
              muVec.push_back(muNodeId);

              const uint32_t chiNodeId = nextNode();
              const uint32_t chiVersion = nextVersion(&F, memRegId);
              auto *chiNode = new StoreChiSVFGNode(
                  chiNodeId, icfgNode, nullptr, memRegId,
                  SVFGNodeBS{getOrCreateUnknownObjId()}, chiVersion);
              svfg->addNode(chiNode);
              chiVec.push_back(chiNodeId);
              svfg->setMSSADef(memRegId, chiNode, chiVersion);
            } else {
              for (uint32_t objId : objIds) {
                const uint32_t memRegId = getOrCreateMemRegForObject(objId);
                SVFGNodeBS pts{objId};

                const uint32_t muNodeId = nextNode();
                auto *muNode = new LoadMuSVFGNode(muNodeId, icfgNode, nullptr,
                                                  memRegId, pts);
                svfg->addNode(muNode);
                muVec.push_back(muNodeId);

                const uint32_t chiNodeId = nextNode();
                const uint32_t chiVersion = nextVersion(&F, memRegId);
                auto *chiNode = new StoreChiSVFGNode(
                    chiNodeId, icfgNode, nullptr, memRegId, pts, chiVersion);
                svfg->addNode(chiNode);
                chiVec.push_back(chiNodeId);
                svfg->setMSSADef(memRegId, chiNode, chiVersion);
              }
            }
          }
        }

        // Create CallMu and CallChi nodes for call sites
        if (const CallBase *call = dyn_cast<CallBase>(&inst)) {
          // Handle intrinsic calls that affect memory
          if (const IntrinsicInst *intrinsic = dyn_cast<IntrinsicInst>(call)) {
            Intrinsic::ID id = intrinsic->getIntrinsicID();
            // Handle memory-affecting intrinsics
            switch (id) {
            case Intrinsic::memcpy:
            case Intrinsic::memmove:
            case Intrinsic::memset: {
              // Find ICFG node for this intrinsic call
              const ICFGNode *icfgNode = nullptr;
              for (auto &pair : *icfg) {
                if (IntraBlockNode *blockNode =
                        dyn_cast<IntraBlockNode>(pair.second)) {
                  if (blockNode->getBasicBlock() == &bb) {
                    icfgNode = blockNode;
                    break;
                  }
                }
              }

              // These intrinsics modify memory - create StoreChi-like nodes
              // Get destination pointer (first argument)
              if (call->arg_size() >= 1) {
                const Value *dstPtr = call->getArgOperand(0);
                if (dstPtr->getType()->isPointerTy()) {
                  std::vector<const void *> ptsVoid = getPointsToSet(dstPtr);
                  SVFGNodeBS dstObjIds = convertPTAObjectsToObjIDs(ptsVoid);
                  std::vector<uint32_t> dstChiNodes;
                  if (dstObjIds.empty()) {
                    const uint32_t memRegId = getOrCreateMemReg(dstPtr);
                    const uint32_t chiNodeId = nextNode();
                    const uint32_t chiVersion = nextVersion(&F, memRegId);
                    auto *chiNode = new StoreChiSVFGNode(
                        chiNodeId, icfgNode, nullptr, memRegId, SVFGNodeBS{},
                        chiVersion);
                    svfg->addNode(chiNode);
                    dstChiNodes.push_back(chiNodeId);
                    svfg->setMSSADef(memRegId, chiNode, chiVersion);
                  } else {
                    for (uint32_t objId : dstObjIds) {
                      const uint32_t memRegId =
                          getOrCreateMemRegForObject(objId);
                      const uint32_t chiNodeId = nextNode();
                      const uint32_t chiVersion = nextVersion(&F, memRegId);
                      SVFGNodeBS pts{objId};
                      auto *chiNode =
                          new StoreChiSVFGNode(chiNodeId, icfgNode, nullptr,
                                               memRegId, pts, chiVersion);
                      svfg->addNode(chiNode);
                      dstChiNodes.push_back(chiNodeId);
                      svfg->setMSSADef(memRegId, chiNode, chiVersion);
                    }
                  }

                  // Also create LoadMu for source (if memcpy/memmove)
                  if (id == Intrinsic::memcpy || id == Intrinsic::memmove) {
                    if (call->arg_size() >= 2) {
                      const Value *srcPtr = call->getArgOperand(1);
                      if (srcPtr->getType()->isPointerTy()) {
                        std::vector<const void *> srcPtsVoid =
                            getPointsToSet(srcPtr);
                        SVFGNodeBS srcObjIds =
                            convertPTAObjectsToObjIDs(srcPtsVoid);
                        std::vector<uint32_t> srcMuNodes;
                        if (srcObjIds.empty()) {
                          const uint32_t memRegId = getOrCreateMemReg(srcPtr);
                          const uint32_t muNodeId = nextNode();
                          auto *muNode =
                              new LoadMuSVFGNode(muNodeId, icfgNode, nullptr,
                                                 memRegId, SVFGNodeBS{});
                          svfg->addNode(muNode);
                          srcMuNodes.push_back(muNodeId);
                        } else {
                          for (uint32_t objId : srcObjIds) {
                            const uint32_t memRegId =
                                getOrCreateMemRegForObject(objId);
                            const uint32_t muNodeId = nextNode();
                            SVFGNodeBS pts{objId};
                            auto *muNode = new LoadMuSVFGNode(
                                muNodeId, icfgNode, nullptr, memRegId, pts);
                            svfg->addNode(muNode);
                            srcMuNodes.push_back(muNodeId);
                          }
                        }

                        // Connect all src MU to all dst CHI to represent the
                        // copy.
                        for (uint32_t muId : srcMuNodes) {
                          SVFGNode *muNode = svfg->getNode(muId);
                          if (!muNode)
                            continue;
                          for (uint32_t chiId : dstChiNodes) {
                            SVFGNode *chiNode = svfg->getNode(chiId);
                            if (!chiNode)
                              continue;
                            auto *muMem = dyn_cast<MSSASVFGNode>(muNode);
                            auto *chiMem = dyn_cast<MSSASVFGNode>(chiNode);
                            if (!muMem || !chiMem)
                              continue;
                            // Only connect matching memory regions (same
                            // object).
                            if (muMem->getMemReg() != chiMem->getMemReg())
                              continue;
                            SVFGNodeBS edgePts = intersectPointsToSets(
                                muMem->getDefSVFVars(), chiMem->getDefSVFVars(),
                                getOrCreateUnknownObjId());
                            if (edgePts.empty())
                              edgePts.insert(getOrCreateUnknownObjId());
                            svfg->addEdge(muNode, chiNode, SVFGEdgeK::IntraCopy,
                                          nullptr, edgePts);
                          }
                        }
                      }
                    }
                  }
                }
              }
              continue; // Skip normal CallMu/CallChi creation
            }
            case Intrinsic::dbg_value:
            case Intrinsic::dbg_declare:
            case Intrinsic::dbg_label:
            case Intrinsic::lifetime_start:
            case Intrinsic::lifetime_end:
            case Intrinsic::invariant_start:
            case Intrinsic::invariant_end:
              // These don't affect memory - skip
              continue;
            default:
              // Other intrinsics - conservative: treat as normal call
              break;
            }
          }

          // Create upstream-style ActualIn/ActualOut nodes from the callsite's
          // computed ref/mod regions rather than standalone CallMu/CallChi
          // wrapper nodes.
          auto readRegsIt = callReadRegions.find(call);
          auto writeRegsIt = callWriteRegions.find(call);
          if (readRegsIt == callReadRegions.end() &&
              writeRegsIt == callWriteRegions.end())
            continue;

          // Find ICFG node
          const ICFGNode *icfgNode = nullptr;
          for (auto &pair : *icfg) {
            if (IntraBlockNode *blockNode =
                    dyn_cast<IntraBlockNode>(pair.second)) {
              if (blockNode->getBasicBlock() == &bb) {
                icfgNode = blockNode;
                break;
              }
            }
          }

          auto &muVec = callToMuNodes[call];
          auto &chiVec = callToChiNodes[call];
          if (readRegsIt != callReadRegions.end()) {
            for (const auto &entry : readRegsIt->second) {
              const uint32_t memRegId = entry.first;
              const SVFGNodeBS &pts = entry.second;
              const uint32_t actualInId = nextNode();
              auto *actualIn =
                  new ActualInSVFGNode(actualInId, icfgNode, call, memRegId, pts);
              svfg->addNode(actualIn);
              svfg->addActualIn(call, actualIn);
              muVec.push_back(actualInId);
            }
          }
          if (writeRegsIt != callWriteRegions.end()) {
            for (const auto &entry : writeRegsIt->second) {
              const uint32_t memRegId = entry.first;
              const SVFGNodeBS &pts = entry.second;
              const uint32_t actualOutId = nextNode();
              const uint32_t actualOutVersion = nextVersion(&F, memRegId);
              auto *actualOut = new ActualOutSVFGNode(
                  actualOutId, icfgNode, call, memRegId, pts, actualOutVersion);
              svfg->addNode(actualOut);
              svfg->addActualOut(call, actualOut);
              chiVec.push_back(actualOutId);
              svfg->setMSSADef(memRegId, actualOut, actualOutVersion);
            }
          }

          if (readRegsIt == callReadRegions.end())
            callToMuNodes.erase(call);
          if (writeRegsIt == callWriteRegions.end())
            callToChiNodes.erase(call);
        }
      }
    }
  }

  for (const Function &F : *M) {
    if (F.isDeclaration())
      continue;

    auto summaryIt = summaries.find(&F);
    if (summaryIt == summaries.end())
      continue;
    const FunctionMemorySummary &summary = summaryIt->second;
    const ICFGNode *entryICFGNode = findICFGNodeForBlock(icfg, &F.getEntryBlock());

    std::unordered_map<uint32_t, SVFGNodeBS> formalInRegs = summary.readGlobals;
    std::unordered_map<uint32_t, SVFGNodeBS> formalOutRegs = summary.readGlobals;
    mergeRegions(formalInRegs, summary.writeGlobals);
    mergeRegions(formalOutRegs, summary.writeGlobals);

    auto addFormalArgRegions = [&](unsigned argIdx, MemRegPtsMap &dst) {
      if (argIdx >= F.arg_size())
        return;
      const Argument *arg = F.getArg(argIdx);
      if (!arg || !arg->getType()->isPointerTy())
        return;

      std::vector<const void *> ptsVoid = getPointsToSet(arg);
      SVFGNodeBS objIds = convertPTAObjectsToObjIDs(ptsVoid);
      if (objIds.empty()) {
        addRegion(dst, getOrCreateMemReg(arg),
                  SVFGNodeBS{getOrCreateUnknownObjId()});
        return;
      }
      for (uint32_t objId : objIds) {
        addRegion(dst, getOrCreateMemRegForObject(objId), SVFGNodeBS{objId});
      }
    };

    for (unsigned argIdx : summary.readArgs)
      addFormalArgRegions(argIdx, formalInRegs);
    for (unsigned argIdx : summary.readArgs)
      addFormalArgRegions(argIdx, formalOutRegs);
    for (unsigned argIdx : summary.writeArgs)
      addFormalArgRegions(argIdx, formalInRegs);
    for (unsigned argIdx : summary.writeArgs)
      addFormalArgRegions(argIdx, formalOutRegs);

    auto entryRegIt = funcEntryChiMemRegs.find(&F);
    if (entryRegIt != funcEntryChiMemRegs.end()) {
      for (uint32_t memReg : entryRegIt->second) {
        if (formalInRegs.find(memReg) == formalInRegs.end())
          formalInRegs.emplace(memReg, getPtsForMemReg(memReg));
      }
    }

    for (const auto &entry : formalInRegs) {
      const uint32_t formalInId = nextNode();
      const uint32_t formalInVersion = nextVersion(&F, entry.first);
      auto *formalIn =
          new FormalInSVFGNode(formalInId, entryICFGNode, &F, entry.first,
                               entry.second, formalInVersion);
      svfg->addNode(formalIn);
      svfg->addFormalIn(&F, formalIn);
      funcEntryChi[&F].push_back(formalInId);
      svfg->setMSSADef(entry.first, formalIn, formalInVersion);
    }

    for (const auto &entry : formalOutRegs) {
      const uint32_t formalOutId = nextNode();
      auto *formalOut = new FormalOutSVFGNode(formalOutId, entryICFGNode, &F,
                                              entry.first, entry.second);
      svfg->addNode(formalOut);
      svfg->addFormalOut(&F, formalOut);
    }
  }
}

void SVFGBuilder::buildMemoryPHINodes() {
  if (!config.buildMSSA)
    return;

  const Module *M = getModuleFromICFG(icfg);
  if (!M)
    return;

  for (const Function &F : *M) {
    if (F.isDeclaration())
      continue;

    std::unordered_map<uint32_t, SVFGNode *> formalInByReg;
    std::set<uint32_t> memRegsWithDefs;
    std::unordered_map<const BasicBlock *, std::unordered_map<uint32_t, SVFGNode *>>
        localDefs;

    for (uint32_t formalInId : funcEntryChi[&F]) {
      if (auto *formalIn =
              dyn_cast<FormalInSVFGNode>(svfg->getNode(formalInId))) {
        formalInByReg[formalIn->getMemReg()] = formalIn;
        memRegsWithDefs.insert(formalIn->getMemReg());
      }
    }

    for (const BasicBlock &bb : F) {
      auto &blockDefs = localDefs[&bb];
      for (const Instruction &inst : bb) {
        if (const auto *store = dyn_cast<StoreInst>(&inst)) {
          auto chiIt = storeToChiNodes.find(store);
          if (chiIt != storeToChiNodes.end()) {
            for (uint32_t chiId : chiIt->second) {
              if (auto *chi =
                      dyn_cast<StoreChiSVFGNode>(svfg->getNode(chiId))) {
                blockDefs[chi->getMemReg()] = chi;
                memRegsWithDefs.insert(chi->getMemReg());
              }
            }
          }
        }

        auto atomicChiIt = atomicToChiNodes.find(&inst);
        if (atomicChiIt != atomicToChiNodes.end()) {
          for (uint32_t chiId : atomicChiIt->second) {
            if (auto *chi = dyn_cast<StoreChiSVFGNode>(svfg->getNode(chiId))) {
              blockDefs[chi->getMemReg()] = chi;
              memRegsWithDefs.insert(chi->getMemReg());
            }
          }
        }

        if (const auto *call = dyn_cast<CallBase>(&inst)) {
          auto chiIt = callToChiNodes.find(call);
          if (chiIt != callToChiNodes.end()) {
            for (uint32_t chiId : chiIt->second) {
              if (auto *actualOut =
                      dyn_cast<ActualOutSVFGNode>(svfg->getNode(chiId))) {
                blockDefs[actualOut->getMemReg()] = actualOut;
                memRegsWithDefs.insert(actualOut->getMemReg());
              }
            }
          }
        }
      }
    }

    auto computeExitDefs =
        [&](std::unordered_map<const BasicBlock *,
                               std::unordered_map<uint32_t, SVFGNode *>> &out) {
          out.clear();
          std::queue<const BasicBlock *> worklist;
          std::set<const BasicBlock *> inQueue;
          worklist.push(&F.getEntryBlock());
          inQueue.insert(&F.getEntryBlock());

          while (!worklist.empty()) {
            const BasicBlock *bb = worklist.front();
            worklist.pop();
            inQueue.erase(bb);

            std::unordered_map<uint32_t, SVFGNode *> current;
            if (bb == &F.getEntryBlock()) {
              current = formalInByReg;
            } else {
              for (const BasicBlock *pred : predecessors(bb)) {
                auto predIt = out.find(pred);
                if (predIt == out.end())
                  continue;
                for (const auto &pair : predIt->second) {
                  const uint32_t memReg = pair.first;
                  SVFGNode *def = pair.second;
                  auto phiIt = bbToMemPhi[bb].find(memReg);
                  if (phiIt != bbToMemPhi[bb].end()) {
                    current[memReg] = svfg->getNode(phiIt->second);
                    continue;
                  }
                  if (current.find(memReg) == current.end())
                    current[memReg] = def;
                }
              }
            }

            auto phiMapIt = bbToMemPhi.find(bb);
            if (phiMapIt != bbToMemPhi.end()) {
              for (const auto &phiPair : phiMapIt->second)
                current[phiPair.first] = svfg->getNode(phiPair.second);
            }

            auto localIt = localDefs.find(bb);
            if (localIt != localDefs.end()) {
              for (const auto &pair : localIt->second)
                current[pair.first] = pair.second;
            }

            const bool changed =
                (out.find(bb) == out.end()) || out[bb] != current;
            if (!changed)
              continue;

            out[bb] = std::move(current);
            for (const BasicBlock *succ : successors(bb)) {
              if (inQueue.insert(succ).second)
                worklist.push(succ);
            }
          }
        };

    bool changed = true;
    while (changed) {
      changed = false;

      std::unordered_map<const BasicBlock *,
                         std::unordered_map<uint32_t, SVFGNode *>>
          exitDefs;
      computeExitDefs(exitDefs);

      for (const BasicBlock &bb : F) {
        const unsigned numPreds =
            std::distance(pred_begin(&bb), pred_end(&bb));
        if (numPreds < 2)
          continue;

        for (uint32_t memReg : memRegsWithDefs) {
          if (bbToMemPhi[&bb].count(memReg))
            continue;

          std::vector<SVFGNode *> perPredDefs;
          std::set<SVFGNode *> distinctDefs;
          for (const BasicBlock *pred : predecessors(&bb)) {
            SVFGNode *incomingDef = nullptr;
            auto predIt = exitDefs.find(pred);
            if (predIt != exitDefs.end()) {
              auto defIt = predIt->second.find(memReg);
              if (defIt != predIt->second.end())
                incomingDef = defIt->second;
            }
            if (!incomingDef) {
              auto entryIt = formalInByReg.find(memReg);
              if (entryIt != formalInByReg.end())
                incomingDef = entryIt->second;
            }
            perPredDefs.push_back(incomingDef);
            if (incomingDef)
              distinctDefs.insert(incomingDef);
          }

          if (distinctDefs.size() < 2)
            continue;

          const uint32_t phiNodeId = createMemoryPHI(memReg, &bb);
          auto *phiNode =
              dyn_cast<IntraMSSAPhiSVFGNode>(svfg->getNode(phiNodeId));
          if (!phiNode)
            continue;

          if (phiNode->getDefSVFVars().empty()) {
            if (SVFGNode *seed = *distinctDefs.begin()) {
              if (auto *seedMem = dyn_cast<MSSASVFGNode>(seed)) {
                if (auto *phiPts =
                        const_cast<SVFGNodeBS *>(phiNode->getPointsTo())) {
                  *phiPts = seedMem->getDefSVFVars();
                }
              }
            }
          }

          uint32_t predIdx = 0;
          for (SVFGNode *incomingDef : perPredDefs) {
            if (!incomingDef) {
              ++predIdx;
              continue;
            }
            SVFGNodeBS edgePts = phiNode->getDefSVFVars();
            if (edgePts.empty())
              edgePts.insert(getOrCreateUnknownObjId());
            svfg->addEdge(incomingDef, phiNode, SVFGEdgeK::IntraPhi, nullptr,
                          edgePts);
            if (auto *defMem = dyn_cast<MSSASVFGNode>(incomingDef)) {
              phiNode->setOpVer(predIdx, defMem->getMemReg(),
                                defMem->getSSAVersion());
            }
            ++predIdx;
          }

          changed = true;
        }
      }
    }
  }
}

void SVFGBuilder::buildInterproceduralMemoryPHINodes() {
  // DISABLED – this function used to insert InterMSSAPhiSVFGNode nodes
  // between FormalIn and FormalOut (and between ActualIn and ActualOut),
  // which is INCORRECT:
  //
  //   FormalIn → [InterPhi] → FormalOut
  //
  // FormalIn represents the memory state AT ENTRY; FormalOut represents the
  // state AT EXIT after all stores in the body.  Inserting a synthetic
  // inter-procedural PHI between them would bypass all intra-procedural
  // StoreChi/PHI nodes and create direct def-use edges that skip the actual
  // program flow, producing spurious value-flow paths.
  //
  // The correct inter-procedural memory flow is already established by:
  //   • buildCallEdges:   ActualIn  → FormalIn  (CallAIn edge)
  //   • buildReturnEdges: FormalOut → ActualOut (RetAOut edge)
  //   • connectMemorySSAEdges: stores/loads connected inside each function
  //
  // If you need to model a callee that passes memory through unchanged, that
  // is correctly handled by having no StoreChi in the callee body, which
  // means the reaching-def chain continues from FormalIn through FormalOut
  // without any additional node.  No synthetic PHI is needed.
  return;
}

void SVFGBuilder::connectMemorySSAEdges() {
  if (!config.buildMSSA)
    return;

  const Module *M = getModuleFromICFG(icfg);
  if (!M)
    return;

  auto getLoadStmtNode = [&](const llvm::LoadInst *li) -> SVFGNode * {
    if (!li)
      return nullptr;
    auto it = loadToLoadNode.find(li);
    return (it != loadToLoadNode.end()) ? svfg->getNode(it->second) : nullptr;
  };

  auto getStoreStmtNode = [&](const llvm::StoreInst *si) -> SVFGNode * {
    if (!si)
      return nullptr;
    auto it = storeToStoreNode.find(si);
    return (it != storeToStoreNode.end()) ? svfg->getNode(it->second) : nullptr;
  };

  // Map: memory region -> last def (StoreChi/FormalIn/ActualOut) in each block.
  std::unordered_map<const BasicBlock *,
                     std::unordered_map<uint32_t, SVFGNode *>>
      lastDefInBlock;

  // Map: memory region -> FormalIn node for function entry.
  std::unordered_map<const Function *, std::unordered_map<uint32_t, SVFGNode *>>
      funcEntryChiMap;

  // First pass: collect function-entry defs and last defs in each block.
  for (const Function &F : *M) {
    if (F.isDeclaration())
      continue;

    for (uint32_t formalInId : funcEntryChi[&F]) {
      SVFGNode *formalInNode = svfg->getNode(formalInId);
      if (auto *formalIn = dyn_cast<FormalInSVFGNode>(formalInNode)) {
        uint32_t memReg = formalIn->getMemReg();
        funcEntryChiMap[&F][memReg] = formalInNode;
      }
    }

    // Track last defs in each basic block
    for (const BasicBlock &bb : F) {
      for (const Instruction &inst : bb) {
        if (const StoreInst *store = dyn_cast<StoreInst>(&inst)) {
          auto chiIt = storeToChiNodes.find(store);
          if (chiIt != storeToChiNodes.end()) {
            for (uint32_t chiId : chiIt->second) {
              SVFGNode *chiNode = svfg->getNode(chiId);
              if (auto *chi = dyn_cast<StoreChiSVFGNode>(chiNode)) {
                lastDefInBlock[&bb][chi->getMemReg()] = chiNode;
              }
            }
          }
        }
      }
    }
  }

  // Second pass: connect LoadMu nodes to their reaching defs
  // Track last def per memory region per function (needed for fourth pass)
  std::unordered_map<
      const Function *,
      std::unordered_map<const BasicBlock *,
                         std::unordered_map<uint32_t, SVFGNode *>>>
      lastDefAtBlockMap;

  for (const Function &F : *M) {
    if (F.isDeclaration())
      continue;

    // Track last def per memory region as we traverse blocks
    // Use a worklist-based approach to handle control flow properly
    std::unordered_map<const BasicBlock *,
                       std::unordered_map<uint32_t, SVFGNode *>>
        &lastDefAtBlock = lastDefAtBlockMap[&F];

    // Initialize entry block with EntryChi nodes
    for (auto &pair : funcEntryChiMap[&F]) {
      lastDefAtBlock[&F.getEntryBlock()][pair.first] = pair.second;
    }

    // Worklist fixpoint over CFG blocks to correctly propagate backedges.
    std::queue<const BasicBlock *> worklist;
    std::set<const BasicBlock *> inQueue;
    worklist.push(&F.getEntryBlock());
    inQueue.insert(&F.getEntryBlock());

    // Process blocks using worklist (handles control flow properly)
    while (!worklist.empty()) {
      const BasicBlock *bb = worklist.front();
      worklist.pop();
      inQueue.erase(bb);

      // Get last defs at entry of this block
      std::unordered_map<uint32_t, SVFGNode *> lastDef;

      // If this is the entry block, use EntryChi nodes
      if (bb == &F.getEntryBlock()) {
        for (auto &pair : funcEntryChiMap[&F]) {
          lastDef[pair.first] = pair.second;
        }
      } else {
        // For other blocks, merge defs from all predecessors
        for (const BasicBlock *pred : predecessors(bb)) {
          auto predDefsIt = lastDefAtBlock.find(pred);
          if (predDefsIt != lastDefAtBlock.end()) {
            for (auto &pair : predDefsIt->second) {
              uint32_t memReg = pair.first;
              SVFGNode *def = pair.second;

              // If multiple predecessors have defs for same region, use PHI if
              // exists
              auto phiIt = bbToMemPhi[bb].find(memReg);
              if (phiIt != bbToMemPhi[bb].end()) {
                // Use PHI node as the def
                lastDef[memReg] = svfg->getNode(phiIt->second);
              } else {
                // Check if we already have a def from another predecessor
                auto existingDefIt = lastDef.find(memReg);
                if (existingDefIt == lastDef.end()) {
                  // First def for this region from this block
                  lastDef[memReg] = def;
                } else if (existingDefIt->second != def) {
                  // Multiple predecessors have different defs and no PHI:
                  // create one on demand to avoid losing reaching definitions.
                  uint32_t phiId = createMemoryPHI(memReg, bb);
                  SVFGNode *phiNode = svfg->getNode(phiId);
                  if (phiNode) {
                    SVFGNodeBS edgePts = phiNode->getDefSVFVars();
                    if (edgePts.empty())
                      edgePts.insert(getOrCreateUnknownObjId());
                    svfg->addEdge(existingDefIt->second, phiNode,
                                  SVFGEdgeK::IntraPhi, nullptr, edgePts);
                    svfg->addEdge(def, phiNode, SVFGEdgeK::IntraPhi, nullptr,
                                  edgePts);
                    lastDef[memReg] = phiNode;
                    // Bug #8 fix: the on-demand PHI changes the out-state of
                    // this block, so successors must be re-queued. The old
                    // code updated lastDef but never pushed successors back
                    // onto the worklist, so the PHI's reaching-def was never
                    // propagated forward.
                    for (const BasicBlock *succ : successors(bb)) {
                      if (inQueue.insert(succ).second) {
                        worklist.push(succ);
                      }
                    }
                  }
                }
                // If same def, no change needed
              }
            }
          }
        }
      }

      // Process instructions in this block
      for (const Instruction &inst : *bb) {
        if (const LoadInst *load = dyn_cast<LoadInst>(&inst)) {
          auto muIt = loadToMuNodes.find(load);
          if (muIt != loadToMuNodes.end()) {
            for (uint32_t muId : muIt->second) {
              SVFGNode *muNode = svfg->getNode(muId);
              auto *mu = dyn_cast<LoadMuSVFGNode>(muNode);
              if (!mu)
                continue;
              const uint32_t memReg = mu->getMemReg();

              SVFGNode *reachingDef = nullptr;
              auto phiIt = bbToMemPhi[bb].find(memReg);
              if (phiIt != bbToMemPhi[bb].end()) {
                reachingDef = svfg->getNode(phiIt->second);
              } else {
                auto defIt = lastDef.find(memReg);
                if (defIt != lastDef.end() && defIt->second) {
                  reachingDef = defIt->second;
                } else {
                  auto entryIt = funcEntryChiMap[&F].find(memReg);
                  if (entryIt != funcEntryChiMap[&F].end()) {
                    reachingDef = entryIt->second;
                  }
                }
              }

              if (reachingDef) {
                SVFGNodeBS edgePts = muNode->getDefSVFVars();
                if (edgePts.empty())
                  edgePts.insert(getOrCreateUnknownObjId());
                svfg->addEdge(reachingDef, muNode, SVFGEdgeK::IntraMu, nullptr,
                              edgePts);

                // Design (A): expose SVF-style guarded memory value-flow on
                // the statement Load node for DDA.
                if (const LoadInst *li = mu->getLoadInst()) {
                  if (SVFGNode *loadStmt = getLoadStmtNode(li)) {
                    svfg->addEdge(reachingDef, loadStmt,
                                  SVFGEdgeK::IntraIndirect, nullptr, edgePts);
                  }
                }
              }
            }
          }
        }
        auto atomicMuIt = atomicToMuNodes.find(&inst);
        if (atomicMuIt != atomicToMuNodes.end()) {
          for (uint32_t muId : atomicMuIt->second) {
            SVFGNode *muNode = svfg->getNode(muId);
            auto *mu = dyn_cast<LoadMuSVFGNode>(muNode);
            if (!mu)
              continue;
            const uint32_t memReg = mu->getMemReg();

            SVFGNode *reachingDef = nullptr;
            auto phiIt = bbToMemPhi[bb].find(memReg);
            if (phiIt != bbToMemPhi[bb].end()) {
              reachingDef = svfg->getNode(phiIt->second);
            } else {
              auto defIt = lastDef.find(memReg);
              if (defIt != lastDef.end() && defIt->second) {
                reachingDef = defIt->second;
              } else {
                auto entryIt = funcEntryChiMap[&F].find(memReg);
                  if (entryIt != funcEntryChiMap[&F].end()) {
                    reachingDef = entryIt->second;
                  }
                }
            }

            if (reachingDef) {
              SVFGNodeBS edgePts = muNode->getDefSVFVars();
              if (edgePts.empty())
                edgePts.insert(getOrCreateUnknownObjId());
              svfg->addEdge(reachingDef, muNode, SVFGEdgeK::IntraMu, nullptr,
                            edgePts);

              if (const LoadInst *li = mu->getLoadInst()) {
                if (SVFGNode *loadStmt = getLoadStmtNode(li)) {
                  svfg->addEdge(reachingDef, loadStmt, SVFGEdgeK::IntraIndirect,
                                nullptr, edgePts);
                }
              }
            }
          }
        }

        if (const StoreInst *store = dyn_cast<StoreInst>(&inst)) {
          auto chiIt = storeToChiNodes.find(store);
          if (chiIt != storeToChiNodes.end()) {
            for (uint32_t chiId : chiIt->second) {
              SVFGNode *chiNode = svfg->getNode(chiId);
              auto *chi = dyn_cast<StoreChiSVFGNode>(chiNode);
              if (!chi)
                continue;
              const uint32_t memReg = chi->getMemReg();
              auto prevIt = lastDef.find(memReg);
              if (prevIt != lastDef.end() && prevIt->second &&
                  prevIt->second != chiNode) {
                SVFGNodeBS edgePts = chiNode->getDefSVFVars();
                if (edgePts.empty())
                  edgePts.insert(getOrCreateUnknownObjId());
                svfg->addEdge(prevIt->second, chiNode, SVFGEdgeK::IntraChi,
                              nullptr, edgePts);

                // Design (A): guarded indirect flow into Store statement.
                if (SVFGNode *storeStmt = getStoreStmtNode(store)) {
                  svfg->addEdge(prevIt->second, storeStmt,
                                SVFGEdgeK::IntraIndirect, nullptr, edgePts);
                }
              }
              lastDef[memReg] = chiNode;
            }
          }
        }
        auto atomicChiIt = atomicToChiNodes.find(&inst);
        if (atomicChiIt != atomicToChiNodes.end()) {
          for (uint32_t chiId : atomicChiIt->second) {
            SVFGNode *chiNode = svfg->getNode(chiId);
            auto *chi = dyn_cast<StoreChiSVFGNode>(chiNode);
            if (!chi)
              continue;
            const uint32_t memReg = chi->getMemReg();
            auto prevIt = lastDef.find(memReg);
            if (prevIt != lastDef.end() && prevIt->second &&
                prevIt->second != chiNode) {
              SVFGNodeBS edgePts = chiNode->getDefSVFVars();
              if (edgePts.empty())
                edgePts.insert(getOrCreateUnknownObjId());
              svfg->addEdge(prevIt->second, chiNode, SVFGEdgeK::IntraChi,
                            nullptr, edgePts);

              if (const StoreInst *si = chi->getStoreInst()) {
                if (SVFGNode *storeStmt = getStoreStmtNode(si)) {
                  svfg->addEdge(prevIt->second, storeStmt,
                                SVFGEdgeK::IntraIndirect, nullptr, edgePts);
                }
              }
            }
            lastDef[memReg] = chiNode;
          }
        }

        if (const auto *call = dyn_cast<CallBase>(&inst)) {
          auto muIt = callToMuNodes.find(call);
          auto chiIt = callToChiNodes.find(call);
          if (muIt != callToMuNodes.end() || chiIt != callToChiNodes.end()) {
            std::unordered_map<uint32_t, SVFGNode *> actualInByReg;
            std::unordered_map<uint32_t, SVFGNode *> actualOutByReg;
            bool hasConcreteCallee = false;

            if (const Function *directCallee = call->getCalledFunction()) {
              hasConcreteCallee = !directCallee->isDeclaration();
            } else if (config.usePointerAnalysis && ptaSolverWrapper &&
                       ptaSolverWrapper->solver && config.resolveIndirectCalls) {
              std::vector<const Function *> callees = getIndirectCallTargets(call);
              callees = filterCalleesByICFG(icfg, call, callees);
              hasConcreteCallee = !callees.empty();
            }

            if (muIt != callToMuNodes.end()) {
              for (uint32_t muId : muIt->second) {
                SVFGNode *muNode = svfg->getNode(muId);
                if (auto *actualIn = dyn_cast<ActualInSVFGNode>(muNode)) {
                  actualInByReg[actualIn->getMemReg()] = muNode;
                }
              }
            }
            if (chiIt != callToChiNodes.end()) {
              for (uint32_t chiId : chiIt->second) {
                SVFGNode *chiNode = svfg->getNode(chiId);
                if (auto *actualOut = dyn_cast<ActualOutSVFGNode>(chiNode)) {
                  actualOutByReg[actualOut->getMemReg()] = chiNode;
                }
              }
            }

            std::set<uint32_t> touchedRegs;
            for (const auto &pair : actualInByReg)
              touchedRegs.insert(pair.first);
            for (const auto &pair : actualOutByReg)
              touchedRegs.insert(pair.first);

            for (uint32_t memReg : touchedRegs) {
              SVFGNode *actualInNode = nullptr;
              SVFGNode *actualOutNode = nullptr;
              auto muNodeIt = actualInByReg.find(memReg);
              if (muNodeIt != actualInByReg.end())
                actualInNode = muNodeIt->second;
              auto chiNodeIt = actualOutByReg.find(memReg);
              if (chiNodeIt != actualOutByReg.end())
                actualOutNode = chiNodeIt->second;

              auto defIt = lastDef.find(memReg);
              SVFGNode *reachingDef =
                  (defIt != lastDef.end()) ? defIt->second : nullptr;

              if (actualInNode && reachingDef) {
                SVFGNodeBS edgePts = actualInNode->getDefSVFVars();
                if (edgePts.empty())
                  edgePts.insert(getOrCreateUnknownObjId());
                svfg->addEdge(reachingDef, actualInNode,
                              SVFGEdgeK::IntraIndirect,
                              nullptr, edgePts);
              }

              if (actualOutNode && !hasConcreteCallee) {
                SVFGNode *fallbackDef = actualInNode ? actualInNode : reachingDef;
                if (fallbackDef) {
                  SVFGNodeBS edgePts = actualOutNode->getDefSVFVars();
                  if (edgePts.empty())
                    edgePts.insert(getOrCreateUnknownObjId());
                  svfg->addEdge(fallbackDef, actualOutNode,
                                SVFGEdgeK::IntraIndirect, nullptr, edgePts);
                }
              }

              if (actualOutNode)
                lastDef[memReg] = actualOutNode;
            }
          }
        }
      }

      auto &prevOut = lastDefAtBlock[bb];
      const bool changed = prevOut != lastDef;
      if (changed) {
        prevOut = lastDef;
      }

      // Revisit successors until block out-state reaches fixpoint.
      if (changed) {
        for (const BasicBlock *succ : successors(bb)) {
          if (inQueue.insert(succ).second) {
            worklist.push(succ);
          }
        }
      } else if (bb == &F.getEntryBlock()) {
        for (const BasicBlock *succ : successors(bb)) {
          if (inQueue.insert(succ).second) {
            worklist.push(succ);
          }
        }
      }
    }
  }

  // Third pass: connect StoreChi to subsequent LoadMu in same block
  for (const Function &F : *M) {
    if (F.isDeclaration())
      continue;

    for (const BasicBlock &bb : F) {
      std::unordered_map<uint32_t, SVFGNode *> lastChiInBlock;

      for (const Instruction &inst : bb) {
        // First, connect any LoadMu to preceding StoreChi
        if (const LoadInst *load = dyn_cast<LoadInst>(&inst)) {
          auto muIt = loadToMuNodes.find(load);
          if (muIt != loadToMuNodes.end()) {
            for (uint32_t muId : muIt->second) {
              SVFGNode *muNode = svfg->getNode(muId);
              auto *mu = dyn_cast<LoadMuSVFGNode>(muNode);
              if (!mu)
                continue;
              const uint32_t memReg = mu->getMemReg();
              auto chiIt = lastChiInBlock.find(memReg);
              if (chiIt != lastChiInBlock.end()) {
                SVFGNodeBS edgePts = muNode->getDefSVFVars();
                if (edgePts.empty())
                  edgePts.insert(getOrCreateUnknownObjId());
                svfg->addEdge(chiIt->second, muNode, SVFGEdgeK::IntraMu,
                              nullptr, edgePts);

                if (const LoadInst *li = mu->getLoadInst()) {
                  if (SVFGNode *loadStmt = getLoadStmtNode(li)) {
                    svfg->addEdge(chiIt->second, loadStmt,
                                  SVFGEdgeK::IntraIndirect, nullptr, edgePts);
                  }
                }
              }
            }
          }
        }
        auto atomicMuIt = atomicToMuNodes.find(&inst);
        if (atomicMuIt != atomicToMuNodes.end()) {
          for (uint32_t muId : atomicMuIt->second) {
            SVFGNode *muNode = svfg->getNode(muId);
            auto *mu = dyn_cast<LoadMuSVFGNode>(muNode);
            if (!mu)
              continue;
            const uint32_t memReg = mu->getMemReg();
            auto chiIt = lastChiInBlock.find(memReg);
            if (chiIt != lastChiInBlock.end()) {
              SVFGNodeBS edgePts = muNode->getDefSVFVars();
              if (edgePts.empty())
                edgePts.insert(getOrCreateUnknownObjId());
              svfg->addEdge(chiIt->second, muNode, SVFGEdgeK::IntraMu, nullptr,
                            edgePts);

              if (const LoadInst *li = mu->getLoadInst()) {
                if (SVFGNode *loadStmt = getLoadStmtNode(li)) {
                  svfg->addEdge(chiIt->second, loadStmt,
                                SVFGEdgeK::IntraIndirect, nullptr, edgePts);
                }
              }
            }
          }
        }

        // Then, update lastChiInBlock for StoreChi
        if (const StoreInst *store = dyn_cast<StoreInst>(&inst)) {
          auto chiIt = storeToChiNodes.find(store);
          if (chiIt != storeToChiNodes.end()) {
            for (uint32_t chiId : chiIt->second) {
              SVFGNode *chiNode = svfg->getNode(chiId);
              if (auto *chi = dyn_cast<StoreChiSVFGNode>(chiNode)) {
                lastChiInBlock[chi->getMemReg()] = chiNode;
              }
            }
          }
        }
        auto atomicChiIt = atomicToChiNodes.find(&inst);
        if (atomicChiIt != atomicToChiNodes.end()) {
          for (uint32_t chiId : atomicChiIt->second) {
            SVFGNode *chiNode = svfg->getNode(chiId);
            if (auto *chi = dyn_cast<StoreChiSVFGNode>(chiNode)) {
              lastChiInBlock[chi->getMemReg()] = chiNode;
            }
          }
        }
      }
    }
  }

  // Fourth pass: connect StoreChi nodes to FormalOut nodes at function exit
  // This tracks memory regions modified by the function
  // Use the lastDefAtBlock map computed in the worklist phase for efficiency
  for (const Function &F : *M) {
    if (F.isDeclaration())
      continue;

    // Get the lastDefAtBlock map for this function from the second pass
    auto funcDefsIt = lastDefAtBlockMap.find(&F);
    if (funcDefsIt == lastDefAtBlockMap.end())
      continue;
    const auto &lastDefAtBlock = funcDefsIt->second;

    // Find all return blocks (blocks with ReturnInst terminator)
    std::vector<const BasicBlock *> returnBlocks;
    for (const BasicBlock &bb : F) {
      if (isa<ReturnInst>(bb.getTerminator())) {
        returnBlocks.push_back(&bb);
      }
    }

    // Connect last defs to FormalOut nodes
    for (auto *formalOut : svfg->getFormalOuts(&F)) {
      if (auto *formalOutMem = dyn_cast<FormalOutSVFGNode>(formalOut)) {
        uint32_t memReg = formalOutMem->getMemReg();

        // Find the last def for this memory region before function exit
        // Use lastDefAtBlock map computed in worklist phase
        std::set<SVFGNode *> exitDefs;

        if (!returnBlocks.empty()) {
          // Collect all distinct defs that reach a return block for this region.
          for (const BasicBlock *retBB : returnBlocks) {
            auto defsIt = lastDefAtBlock.find(retBB);
            if (defsIt != lastDefAtBlock.end()) {
              auto memDefIt = defsIt->second.find(memReg);
              if (memDefIt != defsIt->second.end() && memDefIt->second) {
                exitDefs.insert(memDefIt->second);
              }
            }
          }
        }

        // If no return-block def is available, conservatively fall back to any
        // reachable in-function def for this region.
        if (exitDefs.empty()) {
          for (const BasicBlock &bb : F) {
            auto defsIt = lastDefAtBlock.find(&bb);
            if (defsIt != lastDefAtBlock.end()) {
              auto memDefIt = defsIt->second.find(memReg);
              if (memDefIt != defsIt->second.end() && memDefIt->second) {
                exitDefs.insert(memDefIt->second);
              }
            }
          }
        }

        if (!exitDefs.empty()) {
          for (SVFGNode *exitDef : exitDefs) {
            SVFGNodeBS edgePts = formalOutMem->getDefSVFVars();
            if (edgePts.empty())
              edgePts.insert(getOrCreateUnknownObjId());
            svfg->addEdge(exitDef, formalOutMem, SVFGEdgeK::IntraIndirect,
                          nullptr, edgePts);
          }
        } else {
          // If no def found, connect the function-entry def to indicate the
          // memory region is passed through unchanged.
          auto entryIt = funcEntryChiMap.find(&F);
          if (entryIt != funcEntryChiMap.end()) {
            auto memEntryIt = entryIt->second.find(memReg);
            if (memEntryIt != entryIt->second.end()) {
              SVFGNodeBS edgePts = formalOutMem->getDefSVFVars();
              if (edgePts.empty())
                edgePts.insert(getOrCreateUnknownObjId());
              svfg->addEdge(memEntryIt->second, formalOutMem,
                            SVFGEdgeK::IntraIndirect, nullptr, edgePts);
            }
          }
        }
      }
    }
  }
}

void SVFGBuilder::buildInterproceduralEdges() {
  buildCallEdges();
  buildReturnEdges();
}

void SVFGBuilder::buildCallEdges() {
  for (auto &pair : *icfg) {
    auto *blockNode = dyn_cast<IntraBlockNode>(pair.second);
    if (!blockNode || !blockNode->getBasicBlock())
      continue;

    for (const Instruction &inst : *blockNode->getBasicBlock()) {
      const auto *call = dyn_cast<CallBase>(&inst);
      if (!call)
        continue;

      std::vector<const Function *> callees;
      const Function *directCallee = call->getCalledFunction();
      if (directCallee) {
        if (!directCallee->isDeclaration())
          callees.push_back(directCallee);
      } else if (config.usePointerAnalysis && ptaSolverWrapper &&
                 ptaSolverWrapper->solver && config.resolveIndirectCalls) {
        callees = getIndirectCallTargets(call);
        callees = filterCalleesByICFG(icfg, call, callees);
      }

      for (SVFGNode *actualNode : svfg->getActualParms(call)) {
        auto *actualParm = dyn_cast<ActualParmSVFGNode>(actualNode);
        if (!actualParm)
          continue;
        for (const Function *callee : callees) {
          svfg->markConnectedCallee(call, callee);

          const unsigned actualIdx = actualParm->getParamIndex();
          const bool isVarArgExtra =
              callee->isVarArg() && actualIdx >= callee->arg_size();

          if (isVarArgExtra) {
            // Connect extra arguments (beyond declared params) to
            // VarArgSVFGNode
            for (SVFGNode *formal : svfg->getFormalParms(callee)) {
              auto *varArgNode = dyn_cast<VarArgSVFGNode>(formal);
              if (!varArgNode)
                continue;
              const bool isDirectEdge =
                  (directCallee && directCallee == callee &&
                   !directCallee->isDeclaration());
              if (SVFGEdge *e = svfg->addEdge(actualParm, varArgNode,
                                              isDirectEdge ? SVFGEdgeK::CallDir
                                                           : SVFGEdgeK::CallInd,
                                              call)) {
                if (!isDirectEdge)
                  vfEdgesAtIndCallSite.insert(e);
              }
            }
          } else {
            // Normal parameter matching
            for (SVFGNode *formal : svfg->getFormalParms(callee)) {
              auto *formalParm = dyn_cast<FormalParmSVFGNode>(formal);
              if (!formalParm)
                continue;
              if (formalParm->getParamIndex() != actualIdx)
                continue;
              // Use only CallDir/CallInd – ParamCall is a duplicate and is
              // removed to avoid confusing DDA clients that pattern-match on
              // edge kind.  (SVF uses a single CallDirVF/CallIndVF per pair.)
              const bool isDirectEdge =
                  (directCallee && directCallee == callee &&
                   !directCallee->isDeclaration());
              if (SVFGEdge *e = svfg->addEdge(actualParm, formalParm,
                                              isDirectEdge ? SVFGEdgeK::CallDir
                                                           : SVFGEdgeK::CallInd,
                                              call)) {
                // Track pre-computed indirect edges for spurious-edge
                // filtering.
                if (!isDirectEdge)
                  vfEdgesAtIndCallSite.insert(e);
              }
            }
          }
        }
      }

      for (SVFGNode *actualNode : svfg->getActualIns(call)) {
        auto *actualIn = dyn_cast<ActualInSVFGNode>(actualNode);
        if (!actualIn)
          continue;
        for (const Function *callee : callees) {
          svfg->markConnectedCallee(call, callee);
          for (SVFGNode *formal : svfg->getFormalIns(callee)) {
            auto *formalIn = dyn_cast<FormalInSVFGNode>(formal);
            if (!formalIn)
              continue;
            if (!mayAliasMemoryNodes(actualIn, formalIn))
              continue;
            SVFGNodeBS edgePts =
                intersectPointsToSets(actualIn->getDefSVFVars(),
                                      formalIn->getDefSVFVars(), unknownObjId);
            if (!edgePts.empty())
              svfg->addEdge(actualIn, formalIn, SVFGEdgeK::CallAIn, call,
                            edgePts);
          }
        }
      }
    }
  }
}

void SVFGBuilder::buildReturnEdges() {
  for (auto &pair : *icfg) {
    auto *blockNode = dyn_cast<IntraBlockNode>(pair.second);
    if (!blockNode || !blockNode->getBasicBlock())
      continue;

    for (const Instruction &inst : *blockNode->getBasicBlock()) {
      const auto *call = dyn_cast<CallBase>(&inst);
      if (!call)
        continue;

      std::vector<const Function *> callees;
      const Function *directCallee = call->getCalledFunction();
      if (directCallee) {
        if (!directCallee->isDeclaration())
          callees.push_back(directCallee);
      } else if (config.usePointerAnalysis && ptaSolverWrapper &&
                 ptaSolverWrapper->solver && config.resolveIndirectCalls) {
        callees = getIndirectCallTargets(call);
        callees = filterCalleesByICFG(icfg, call, callees);
      }

      for (SVFGNode *actualNode : svfg->getActualRets(call)) {
        auto *actualRet = dyn_cast<ActualRetSVFGNode>(actualNode);
        if (!actualRet)
          continue;
        for (const Function *callee : callees) {
          svfg->markConnectedCallee(call, callee);
          for (SVFGNode *formal : svfg->getFormalRets(callee)) {
            auto *formalRet = dyn_cast<FormalRetSVFGNode>(formal);
            if (!formalRet)
              continue;
            // Use only RetDir/RetInd – ParamRet is a duplicate (same fix as
            // for ParamCall above).
            const bool isDirectEdge = (directCallee && directCallee == callee &&
                                       !directCallee->isDeclaration());
            if (SVFGEdge *e = svfg->addEdge(formalRet, actualRet,
                                            isDirectEdge ? SVFGEdgeK::RetDir
                                                         : SVFGEdgeK::RetInd,
                                            call)) {
              if (!isDirectEdge)
                vfEdgesAtIndCallSite.insert(e);
            }
          }
        }
      }

      for (SVFGNode *actualNode : svfg->getActualOuts(call)) {
        auto *actualOut = dyn_cast<ActualOutSVFGNode>(actualNode);
        if (!actualOut)
          continue;
        for (const Function *callee : callees) {
          svfg->markConnectedCallee(call, callee);
          for (SVFGNode *formal : svfg->getFormalOuts(callee)) {
            auto *formalOut = dyn_cast<FormalOutSVFGNode>(formal);
            if (!formalOut)
              continue;
            if (!mayAliasMemoryNodes(formalOut, actualOut))
              continue;
            SVFGNodeBS edgePts =
                intersectPointsToSets(formalOut->getDefSVFVars(),
                                      actualOut->getDefSVFVars(), unknownObjId);
            if (!edgePts.empty())
              svfg->addEdge(formalOut, actualOut, SVFGEdgeK::RetAOut, call,
                            edgePts);
          }
        }
      }
    }
  }
}

bool SVFGBuilder::connectCallSiteToCalleeOnTheFly(
    SVFG *g, const CallBase *cs, const Function *callee,
    std::vector<SVFGEdge *> &newEdges) {
  if (!g || !cs || !callee)
    return false;
  if (callee->isDeclaration())
    return false;

  // De-duplicate refinements per (callsite, callee).
  if (!g->markConnectedCallee(cs, callee))
    return false;

  const Function *directCallee = cs->getCalledFunction();
  const bool isDirectEdge = (directCallee && directCallee == callee &&
                             !directCallee->isDeclaration());

  bool created = false;

  // ActualParm -> FormalParm (top-level pointers)
  for (SVFGNode *actualNode : g->getActualParms(cs)) {
    auto *actualParm = dyn_cast<ActualParmSVFGNode>(actualNode);
    if (!actualParm)
      continue;

    const unsigned actualIdx = actualParm->getParamIndex();
    const bool isVarArgExtra =
        callee->isVarArg() && actualIdx >= callee->arg_size();

    if (isVarArgExtra) {
      // Connect extra arguments (beyond declared params) to VarArgSVFGNode
      for (SVFGNode *formalNode : g->getFormalParms(callee)) {
        auto *varArgNode = dyn_cast<VarArgSVFGNode>(formalNode);
        if (!varArgNode)
          continue;
        if (SVFGEdge *e = g->addEdge(
                actualParm, varArgNode,
                isDirectEdge ? SVFGEdgeK::CallDir : SVFGEdgeK::CallInd, cs)) {
          newEdges.push_back(e);
          created = true;
          if (!isDirectEdge)
            vfEdgesAtIndCallSite.insert(e);
        }
      }
    } else {
      // Normal parameter matching
      for (SVFGNode *formalNode : g->getFormalParms(callee)) {
        auto *formalParm = dyn_cast<FormalParmSVFGNode>(formalNode);
        if (!formalParm)
          continue;
        if (formalParm->getParamIndex() != actualIdx)
          continue;
        // Emit only CallDir/CallInd – ParamCall is a duplicate (see
        // buildCallEdges).
        if (SVFGEdge *e = g->addEdge(
                actualParm, formalParm,
                isDirectEdge ? SVFGEdgeK::CallDir : SVFGEdgeK::CallInd, cs)) {
          newEdges.push_back(e);
          created = true;
          if (!isDirectEdge)
            vfEdgesAtIndCallSite.insert(e);
        }
      }
    }
  }

  // ActualIn -> FormalIn (memory)
  for (SVFGNode *actualNode : g->getActualIns(cs)) {
    auto *actualIn = dyn_cast<ActualInSVFGNode>(actualNode);
    if (!actualIn)
      continue;
    for (SVFGNode *formalNode : g->getFormalIns(callee)) {
      auto *formalIn = dyn_cast<FormalInSVFGNode>(formalNode);
      if (!formalIn)
        continue;
      if (!mayAliasMemoryNodes(actualIn, formalIn))
        continue;
      SVFGNodeBS edgePts = intersectPointsToSets(
          actualIn->getDefSVFVars(), formalIn->getDefSVFVars(), unknownObjId);
      if (edgePts.empty())
        continue;
      if (SVFGEdge *e =
              g->addEdge(actualIn, formalIn, SVFGEdgeK::CallAIn, cs, edgePts)) {
        newEdges.push_back(e);
        created = true;
      }
    }
  }

  // FormalRet -> ActualRet (return values)
  for (SVFGNode *actualNode : g->getActualRets(cs)) {
    auto *actualRet = dyn_cast<ActualRetSVFGNode>(actualNode);
    if (!actualRet)
      continue;
    for (SVFGNode *formalNode : g->getFormalRets(callee)) {
      auto *formalRet = dyn_cast<FormalRetSVFGNode>(formalNode);
      if (!formalRet)
        continue;
      // Emit only RetDir/RetInd – ParamRet is a duplicate (see
      // buildReturnEdges).
      if (SVFGEdge *e = g->addEdge(
              formalRet, actualRet,
              isDirectEdge ? SVFGEdgeK::RetDir : SVFGEdgeK::RetInd, cs)) {
        newEdges.push_back(e);
        created = true;
        if (!isDirectEdge)
          vfEdgesAtIndCallSite.insert(e);
      }
    }
  }

  // FormalOut -> ActualOut (memory)
  for (SVFGNode *actualNode : g->getActualOuts(cs)) {
    auto *actualOut = dyn_cast<ActualOutSVFGNode>(actualNode);
    if (!actualOut)
      continue;
    for (SVFGNode *formalNode : g->getFormalOuts(callee)) {
      auto *formalOut = dyn_cast<FormalOutSVFGNode>(formalNode);
      if (!formalOut)
        continue;
      if (!mayAliasMemoryNodes(formalOut, actualOut))
        continue;
      SVFGNodeBS edgePts = intersectPointsToSets(
          formalOut->getDefSVFVars(), actualOut->getDefSVFVars(), unknownObjId);
      if (edgePts.empty())
        continue;
      if (SVFGEdge *e = g->addEdge(formalOut, actualOut, SVFGEdgeK::RetAOut, cs,
                                   edgePts)) {
        newEdges.push_back(e);
        created = true;
      }
    }
  }

  return created;
}

void SVFGBuilder::connectFromGlobalToProgEntry() {
  if (!svfg)
    return;

  const Module *M = getModuleFromICFG(icfg);
  if (!M)
    return;

  SmallVector<const Function *, 8> entryFuncs;
  const Function *mainFunc = M->getFunction("main");
  if (mainFunc && !mainFunc->isDeclaration()) {
    entryFuncs.push_back(mainFunc);
  } else {
    for (const auto &entry : funcEntryChi) {
      if (entry.first && !entry.first->isDeclaration() && !entry.second.empty())
        entryFuncs.push_back(entry.first);
    }
  }
  if (entryFuncs.empty())
    return;

  // Connect global store nodes to entry FormalIn nodes.
  for (SVFGNode *storeNode : svfg->getGlobalStoreNodes()) {
    auto *store = dyn_cast<StoreSVFGNode>(storeNode);
    if (!store)
      continue;

    // Get points-to set of the store's destination
    const llvm::StoreInst *storeInst =
        dyn_cast_or_null<llvm::StoreInst>(store->getInstruction());
    if (!storeInst)
      continue;

    const llvm::Value *ptr = storeInst->getPointerOperand();
    std::vector<const void *> ptsVoid = getPointsToSet(ptr);
    SVFGNodeBS storePts = convertPTAObjectsToObjIDs(ptsVoid);

    // If no points-to info, use unknown object
    if (storePts.empty()) {
      storePts.insert(getOrCreateUnknownObjId());
    }

    for (const Function *entryFunc : entryFuncs) {
      auto entryChiIt = funcEntryChi.find(entryFunc);
      if (entryChiIt == funcEntryChi.end() || entryChiIt->second.empty())
        continue;

      std::unordered_map<uint32_t, SVFGNode *> memRegToFormalIn;
      for (uint32_t chiId : entryChiIt->second) {
        SVFGNode *chiNode = svfg->getNode(chiId);
        if (auto *formalIn = dyn_cast<FormalInSVFGNode>(chiNode)) {
          memRegToFormalIn[formalIn->getMemReg()] = chiNode;
        }
      }

      for (const auto &pair : memRegToFormalIn) {
        SVFGNode *formalInNode = pair.second;
        auto *formalIn = dyn_cast<FormalInSVFGNode>(formalInNode);
        if (!formalIn)
          continue;

        SVFGNodeBS formalInPts = formalIn->getDefSVFVars();
        if (formalInPts.empty()) {
          formalInPts.insert(getOrCreateUnknownObjId());
        }

        SVFGNodeBS intersectPts = intersectPointsToSets(
            storePts, formalInPts, getOrCreateUnknownObjId());

        if (!intersectPts.empty()) {
          svfg->addEdge(storeNode, formalInNode, SVFGEdgeK::IntraIndirect,
                        nullptr, intersectPts);
        }
      }
    }
  }
}
