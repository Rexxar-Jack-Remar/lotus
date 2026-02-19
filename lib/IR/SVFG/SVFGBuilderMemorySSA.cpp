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

          // Create CallMu/CallChi nodes (one per accessed memReg).
          SVFGNodeBS objIds;

          // Get points-to set of all pointer arguments
          for (unsigned i = 0; i < call->arg_size(); ++i) {
            const Value *arg = call->getArgOperand(i);
            if (!arg->getType()->isPointerTy())
              continue;

            std::vector<const void *> ptsVoid = getPointsToSet(arg);
            SVFGNodeBS argObjIds = convertPTAObjectsToObjIDs(ptsVoid);
            objIds.insert(argObjIds.begin(), argObjIds.end());
          }

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
          if (objIds.empty()) {
            const uint32_t memRegId = getOrCreateMemReg(call);
            const uint32_t callMuId = nextNode();
            auto *callMu = new CallMuSVFGNode(callMuId, icfgNode, call,
                                              memRegId, SVFGNodeBS{});
            svfg->addNode(callMu);
            muVec.push_back(callMuId);

            const uint32_t callChiId = nextNode();
            const uint32_t callChiVersion = nextVersion(&F, memRegId);
            auto *callChi =
                new CallChiSVFGNode(callChiId, icfgNode, call, memRegId,
                                    SVFGNodeBS{}, callChiVersion);
            svfg->addNode(callChi);
            chiVec.push_back(callChiId);
            svfg->setMSSADef(memRegId, callChi, callChiVersion);
          } else {
            for (uint32_t objId : objIds) {
              const uint32_t memRegId = getOrCreateMemRegForObject(objId);
              SVFGNodeBS pts{objId};

              const uint32_t callMuId = nextNode();
              auto *callMu =
                  new CallMuSVFGNode(callMuId, icfgNode, call, memRegId, pts);
              svfg->addNode(callMu);
              muVec.push_back(callMuId);

              const uint32_t callChiId = nextNode();
              const uint32_t callChiVersion = nextVersion(&F, memRegId);
              auto *callChi = new CallChiSVFGNode(
                  callChiId, icfgNode, call, memRegId, pts, callChiVersion);
              svfg->addNode(callChi);
              chiVec.push_back(callChiId);
              svfg->setMSSADef(memRegId, callChi, callChiVersion);
            }
          }
        }
      }
    }
  }
}

void SVFGBuilder::buildMemoryPHINodes() {
  if (!config.buildMSSA)
    return;

  const Module *M = getModuleFromICFG(icfg);
  if (!M)
    return;

  // For each function, use iterative algorithm to place PHI nodes correctly
  // This handles all cases: multiple predecessors, single predecessor with PHI,
  // etc.
  for (const Function &F : *M) {
    if (F.isDeclaration())
      continue;

    // Track which memory regions have defs in each basic block
    std::unordered_map<const BasicBlock *, std::set<uint32_t>> bbToMemRegs;

    // First pass: collect memory regions with defs in each block
    for (const BasicBlock &bb : F) {
      for (const Instruction &inst : bb) {
        if (const StoreInst *store = dyn_cast<StoreInst>(&inst)) {
          auto chiIt = storeToChiNodes.find(store);
          if (chiIt != storeToChiNodes.end()) {
            for (uint32_t chiId : chiIt->second) {
              SVFGNode *chiNode = svfg->getNode(chiId);
              if (auto *chi = dyn_cast<StoreChiSVFGNode>(chiNode)) {
                bbToMemRegs[&bb].insert(chi->getMemReg());
              }
            }
          }
        }
        auto atomicChiIt = atomicToChiNodes.find(&inst);
        if (atomicChiIt != atomicToChiNodes.end()) {
          for (uint32_t chiId : atomicChiIt->second) {
            SVFGNode *chiNode = svfg->getNode(chiId);
            if (auto *chi = dyn_cast<StoreChiSVFGNode>(chiNode)) {
              bbToMemRegs[&bb].insert(chi->getMemReg());
            }
          }
        }
      }

      // Also include EntryChi memory regions for entry block
      if (&bb == &F.getEntryBlock()) {
        for (uint32_t entryChiId : funcEntryChi[&F]) {
          SVFGNode *entryChiNode = svfg->getNode(entryChiId);
          if (auto *entryChi = dyn_cast<EntryChiSVFGNode>(entryChiNode)) {
            uint32_t memReg = entryChi->getMemReg();
            bbToMemRegs[&bb].insert(memReg);
          }
        }
      }
    }

    // Track all memory regions that need PHI nodes
    std::set<uint32_t> memRegsWithDefs;
    for (auto &pair : bbToMemRegs) {
      memRegsWithDefs.insert(pair.second.begin(), pair.second.end());
    }
    for (uint32_t entryChiId : funcEntryChi[&F]) {
      SVFGNode *entryChiNode = svfg->getNode(entryChiId);
      if (auto *entryChi = dyn_cast<EntryChiSVFGNode>(entryChiNode)) {
        memRegsWithDefs.insert(entryChi->getMemReg());
      }
    }

    // Iterative algorithm: keep adding PHI nodes until fixpoint
    // A PHI is needed if:
    // 1. Block has multiple predecessors AND
    // 2. At least two predecessors have different defs (or one has def, one has
    // PHI)
    bool changed = true;
    while (changed) {
      changed = false;

      for (const BasicBlock &bb : F) {
        // Skip entry block (no predecessors)
        if (pred_empty(&bb))
          continue;

        // Check if this is a merge point (multiple predecessors)
        unsigned numPreds = std::distance(pred_begin(&bb), pred_end(&bb));
        if (numPreds < 1)
          continue;

        // For each memory region, check if PHI is needed
        for (uint32_t memReg : memRegsWithDefs) {
          // Check if PHI already exists
          auto phiIt = bbToMemPhi[&bb].find(memReg);
          if (phiIt != bbToMemPhi[&bb].end())
            continue;

          // Check if any predecessor has a def or PHI for this region
          bool needsPhi = false;
          std::set<SVFGNode *> incomingDefs;

          for (const BasicBlock *pred : predecessors(&bb)) {
            SVFGNode *incomingDef = nullptr;

            // First check if predecessor has a PHI node
            auto predPhiIt = bbToMemPhi[pred].find(memReg);
            if (predPhiIt != bbToMemPhi[pred].end()) {
              incomingDef = svfg->getNode(predPhiIt->second);
            } else {
              // Look for StoreChi in predecessor (last def)
              for (const Instruction &inst : *pred) {
                if (const StoreInst *store = dyn_cast<StoreInst>(&inst)) {
                  auto chiIt = storeToChiNodes.find(store);
                  if (chiIt != storeToChiNodes.end()) {
                    for (uint32_t chiId : chiIt->second) {
                      SVFGNode *chiNode = svfg->getNode(chiId);
                      if (auto *chi = dyn_cast<StoreChiSVFGNode>(chiNode)) {
                        if (chi->getMemReg() == memReg) {
                          incomingDef = chiNode;
                          // Keep the last one (most recent def)
                        }
                      }
                    }
                  }
                }
                auto atomicChiIt = atomicToChiNodes.find(&inst);
                if (atomicChiIt != atomicToChiNodes.end()) {
                  for (uint32_t chiId : atomicChiIt->second) {
                    SVFGNode *chiNode = svfg->getNode(chiId);
                    if (auto *chi = dyn_cast<StoreChiSVFGNode>(chiNode)) {
                      if (chi->getMemReg() == memReg) {
                        incomingDef = chiNode;
                      }
                    }
                  }
                }
              }

              // If no def found in predecessor, check EntryChi (if pred is
              // entry block)
              if (!incomingDef && pred == &F.getEntryBlock()) {
                for (uint32_t entryChiId : funcEntryChi[&F]) {
                  SVFGNode *entryChiNode = svfg->getNode(entryChiId);
                  if (auto *entryChi =
                          dyn_cast<EntryChiSVFGNode>(entryChiNode)) {
                    if (entryChi->getMemReg() == memReg) {
                      incomingDef = entryChiNode;
                      break;
                    }
                  }
                }
              }
            }

            if (incomingDef) {
              incomingDefs.insert(incomingDef);
            }
          }

          // A memory PHI is needed only at confluence points:
          //   1.  The block has >= 2 predecessors (join point in the CFG).
          //   2.  AND the memory region has at least one reaching definition
          //       along some path (incomingDefs is non-empty).
          //
          // The old condition "numPreds == 1 && predecessor-has-PHI → needsPhi"
          // was WRONG: a block with a single predecessor NEVER needs a PHI in
          // SSA form.  That condition caused cascading/unnecessary PHIs,
          // degrading analysis precision.
          if (numPreds >= 2 && !incomingDefs.empty()) {
            needsPhi = true;
          }

          if (needsPhi) {
            // Get points-to set for this memory region
            SVFGNodeBS ptsSet;
            // Try to get points-to from EntryChi or StoreChi nodes
            for (uint32_t entryChiId : funcEntryChi[&F]) {
              SVFGNode *entryChiNode = svfg->getNode(entryChiId);
              if (auto *entryChi = dyn_cast<EntryChiSVFGNode>(entryChiNode)) {
                if (entryChi->getMemReg() == memReg) {
                  ptsSet = entryChi->getDefSVFVars();
                  break;
                }
              }
            }

            // If no EntryChi found, try to get from any StoreChi
            if (ptsSet.empty()) {
              for (const BasicBlock &searchBB : F) {
                for (const Instruction &inst : searchBB) {
                  if (const StoreInst *store = dyn_cast<StoreInst>(&inst)) {
                    auto chiIt = storeToChiNodes.find(store);
                    if (chiIt != storeToChiNodes.end()) {
                      for (uint32_t chiId : chiIt->second) {
                        SVFGNode *chiNode = svfg->getNode(chiId);
                        if (auto *chi = dyn_cast<StoreChiSVFGNode>(chiNode)) {
                          if (chi->getMemReg() == memReg) {
                            ptsSet = chi->getDefSVFVars();
                            break;
                          }
                        }
                      }
                    }
                  }
                  auto atomicChiIt = atomicToChiNodes.find(&inst);
                  if (atomicChiIt != atomicToChiNodes.end()) {
                    for (uint32_t chiId : atomicChiIt->second) {
                      SVFGNode *chiNode = svfg->getNode(chiId);
                      if (auto *chi = dyn_cast<StoreChiSVFGNode>(chiNode)) {
                        if (chi->getMemReg() == memReg) {
                          ptsSet = chi->getDefSVFVars();
                          break;
                        }
                      }
                    }
                  }
                }
                if (!ptsSet.empty())
                  break;
              }
            }

            // Create memory PHI node
            uint32_t phiNodeId = nextNode();
            // Use per-function versioning to avoid collisions across functions
            uint32_t version = nextVersion(&F, memReg);

            // Find ICFG node for this basic block
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

            auto *phiNode = new IntraMSSAPhiSVFGNode(phiNodeId, icfgNode,
                                                     memReg, version, ptsSet);
            svfg->addNode(phiNode);
            bbToMemPhi[&bb][memReg] = phiNodeId;
            changed = true;

            // Connect PHI to incoming defs from predecessors
            uint32_t predIdx = 0;
            for (const BasicBlock *pred : predecessors(&bb)) {
              SVFGNode *incomingDef = nullptr;

              // Look for last def in predecessor block
              // First check if there's a PHI node in the predecessor
              auto predPhiIt = bbToMemPhi[pred].find(memReg);
              if (predPhiIt != bbToMemPhi[pred].end()) {
                incomingDef = svfg->getNode(predPhiIt->second);
              } else {
                // Look for StoreChi in predecessor (last def)
                SVFGNode *lastChi = nullptr;
                for (const Instruction &inst : *pred) {
                  if (const StoreInst *store = dyn_cast<StoreInst>(&inst)) {
                    auto chiIt = storeToChiNodes.find(store);
                    if (chiIt != storeToChiNodes.end()) {
                      for (uint32_t chiId : chiIt->second) {
                        SVFGNode *chiNode = svfg->getNode(chiId);
                        if (auto *chi = dyn_cast<StoreChiSVFGNode>(chiNode)) {
                          if (chi->getMemReg() == memReg) {
                            lastChi = chiNode;
                          }
                        }
                      }
                    }
                  }
                  auto atomicChiIt = atomicToChiNodes.find(&inst);
                  if (atomicChiIt != atomicToChiNodes.end()) {
                    for (uint32_t chiId : atomicChiIt->second) {
                      SVFGNode *chiNode = svfg->getNode(chiId);
                      if (auto *chi = dyn_cast<StoreChiSVFGNode>(chiNode)) {
                        if (chi->getMemReg() == memReg) {
                          lastChi = chiNode;
                        }
                      }
                    }
                  }
                }
                incomingDef = lastChi;

                // If no def found in predecessor, check EntryChi (if pred is
                // entry block)
                if (!incomingDef && pred == &F.getEntryBlock()) {
                  for (uint32_t entryChiId : funcEntryChi[&F]) {
                    SVFGNode *entryChiNode = svfg->getNode(entryChiId);
                    if (auto *entryChi =
                            dyn_cast<EntryChiSVFGNode>(entryChiNode)) {
                      if (entryChi->getMemReg() == memReg) {
                        incomingDef = entryChiNode;
                        break;
                      }
                    }
                  }
                }
              }

              // Connect incoming def to PHI (use EntryChi as fallback if no def
              // found)
              if (incomingDef) {
                SVFGNodeBS edgePts = phiNode->getDefSVFVars();
                if (edgePts.empty())
                  edgePts.insert(getOrCreateUnknownObjId());
                svfg->addEdge(incomingDef, phiNode, SVFGEdgeK::IntraPhi,
                              nullptr, edgePts);
                // Record the operand version for this predecessor.
                if (auto *defMem = dyn_cast<MSSASVFGNode>(incomingDef)) {
                  phiNode->setOpVer(predIdx, defMem->getMemReg(),
                                    defMem->getSSAVersion());
                }
              } else {
                // No def found - connect to EntryChi if available
                // (conservative)
                for (uint32_t entryChiId : funcEntryChi[&F]) {
                  SVFGNode *entryChiNode = svfg->getNode(entryChiId);
                  if (auto *entryChi =
                          dyn_cast<EntryChiSVFGNode>(entryChiNode)) {
                    if (entryChi->getMemReg() == memReg) {
                      SVFGNodeBS edgePts = phiNode->getDefSVFVars();
                      if (edgePts.empty())
                        edgePts.insert(getOrCreateUnknownObjId());
                      svfg->addEdge(entryChiNode, phiNode, SVFGEdgeK::IntraPhi,
                                    nullptr, edgePts);
                      phiNode->setOpVer(predIdx, entryChi->getMemReg(),
                                        entryChi->getSSAVersion());
                      break;
                    }
                  }
                }
              }
              predIdx++;
            }
          }
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

  // Map: memory region -> last def (StoreChi/EntryChi) in each basic block
  std::unordered_map<const BasicBlock *,
                     std::unordered_map<uint32_t, SVFGNode *>>
      lastDefInBlock;

  // Map: memory region -> EntryChi node for function entry
  std::unordered_map<const Function *, std::unordered_map<uint32_t, SVFGNode *>>
      funcEntryChiMap;

  // First pass: collect EntryChi nodes and last defs in each block
  for (const Function &F : *M) {
    if (F.isDeclaration())
      continue;

    // Collect EntryChi nodes for this function
    for (uint32_t entryChiId : funcEntryChi[&F]) {
      SVFGNode *entryChiNode = svfg->getNode(entryChiId);
      if (auto *entryChi = dyn_cast<EntryChiSVFGNode>(entryChiNode)) {
        uint32_t memReg = entryChi->getMemReg();
        funcEntryChiMap[&F][memReg] = entryChiNode;
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
                // Bug #7 fix: only add IntraMu — do NOT add a second EntryChi
                // edge. The old code added both IntraMu and EntryChi when the
                // reaching def was an EntryChiSVFGNode, causing DDA to traverse
                // the same path twice and produce duplicate points-to entries.
                // IntraMu is the correct edge kind for all reaching-def → Mu
                // connections regardless of whether the def is an EntryChi.
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
              // Bug #7 fix: same as above — only IntraMu, no duplicate
              // EntryChi.
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
          if (muIt != callToMuNodes.end() && chiIt != callToChiNodes.end()) {
            std::unordered_map<uint32_t, SVFGNode *> callMuByReg;
            std::unordered_map<uint32_t, SVFGNode *> callChiByReg;

            for (uint32_t muId : muIt->second) {
              SVFGNode *muNode = svfg->getNode(muId);
              if (auto *mu = dyn_cast<CallMuSVFGNode>(muNode)) {
                callMuByReg[mu->getMemReg()] = muNode;
              }
            }
            for (uint32_t chiId : chiIt->second) {
              SVFGNode *chiNode = svfg->getNode(chiId);
              if (auto *chi = dyn_cast<CallChiSVFGNode>(chiNode)) {
                callChiByReg[chi->getMemReg()] = chiNode;
              }
            }

            for (const auto &pair : callMuByReg) {
              const uint32_t memReg = pair.first;
              SVFGNode *callMuNode = pair.second;
              SVFGNode *callChiNode = nullptr;
              auto chiNodeIt = callChiByReg.find(memReg);
              if (chiNodeIt != callChiByReg.end()) {
                callChiNode = chiNodeIt->second;
              }
              if (!callMuNode || !callChiNode)
                continue;

              // Connect reaching def -> callMu, and callMu -> callChi.
              auto defIt = lastDef.find(memReg);
              if (defIt != lastDef.end() && defIt->second) {
                SVFGNodeBS edgePts = callMuNode->getDefSVFVars();
                if (edgePts.empty())
                  edgePts.insert(getOrCreateUnknownObjId());
                svfg->addEdge(defIt->second, callMuNode, SVFGEdgeK::CallMu,
                              nullptr, edgePts);
              }
              {
                SVFGNodeBS edgePts = callChiNode->getDefSVFVars();
                if (edgePts.empty())
                  edgePts.insert(getOrCreateUnknownObjId());
                svfg->addEdge(callMuNode, callChiNode, SVFGEdgeK::CallChi,
                              nullptr, edgePts);
              }
              lastDef[memReg] = callChiNode;
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
        SVFGNode *lastDef = nullptr;

        if (!returnBlocks.empty()) {
          // Check lastDefAtBlock for each return block, pick the most recent
          for (const BasicBlock *retBB : returnBlocks) {
            auto defsIt = lastDefAtBlock.find(retBB);
            if (defsIt != lastDefAtBlock.end()) {
              auto memDefIt = defsIt->second.find(memReg);
              if (memDefIt != defsIt->second.end() && memDefIt->second) {
                lastDef = memDefIt->second;
                // Keep the last one found (most recent def)
              }
            }
          }
        }

        // If no def found in return blocks, check all blocks for the most
        // recent def
        if (!lastDef) {
          for (const BasicBlock &bb : F) {
            auto defsIt = lastDefAtBlock.find(&bb);
            if (defsIt != lastDefAtBlock.end()) {
              auto memDefIt = defsIt->second.find(memReg);
              if (memDefIt != defsIt->second.end() && memDefIt->second) {
                lastDef = memDefIt->second;
                // Keep updating to get the last one
              }
            }
          }
        }

        // Build explicit function-exit memory use (RetMu) before FormalOut.
        uint32_t retMuId = nextNode();
        uint32_t retMuVersion = nextVersion(&F, memReg);
        SVFGNodeBS ptsSet = formalOutMem->getDefSVFVars();
        auto *retMu = new RetMuSVFGNode(retMuId, nullptr, &F, memReg, ptsSet,
                                        retMuVersion);
        svfg->addNode(retMu);
        funcExitMu[&F].push_back(retMuId);
        svfg->setMSSADef(memReg, retMu, retMuVersion);

        if (lastDef) {
          SVFGNodeBS edgePts = retMu->getDefSVFVars();
          if (edgePts.empty())
            edgePts.insert(getOrCreateUnknownObjId());
          svfg->addEdge(lastDef, retMu, SVFGEdgeK::RetMu, nullptr, edgePts);
        } else {
          // If no def found, connect EntryChi (if exists) to indicate
          // the memory region is passed through without modification.
          auto entryIt = funcEntryChiMap.find(&F);
          if (entryIt != funcEntryChiMap.end()) {
            auto memEntryIt = entryIt->second.find(memReg);
            if (memEntryIt != entryIt->second.end()) {
              SVFGNodeBS edgePts = retMu->getDefSVFVars();
              if (edgePts.empty())
                edgePts.insert(getOrCreateUnknownObjId());
              svfg->addEdge(memEntryIt->second, retMu, SVFGEdgeK::RetMu,
                            nullptr, edgePts);
            }
          }
        }
        {
          SVFGNodeBS edgePts = formalOutMem->getDefSVFVars();
          if (edgePts.empty())
            edgePts.insert(getOrCreateUnknownObjId());
          svfg->addEdge(retMu, formalOutMem, SVFGEdgeK::RetFOut, nullptr,
                        edgePts);
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

          // Count the number of formal parameters for this function
          unsigned numFormalParms = 0;
          for (SVFGNode *formal : svfg->getFormalParms(callee)) {
            auto *formalParm = dyn_cast<FormalParmSVFGNode>(formal);
            if (formalParm) {
              numFormalParms++;
            }
          }

          const unsigned actualIdx = actualParm->getParamIndex();
          const bool isVarArgExtra =
              callee->isVarArg() && actualIdx >= numFormalParms;

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

  // Count the number of formal parameters for this function
  unsigned numFormalParms = 0;
  for (SVFGNode *formal : g->getFormalParms(callee)) {
    auto *formalParm = dyn_cast<FormalParmSVFGNode>(formal);
    if (formalParm) {
      numFormalParms++;
    }
  }

  // ActualParm -> FormalParm (top-level pointers)
  for (SVFGNode *actualNode : g->getActualParms(cs)) {
    auto *actualParm = dyn_cast<ActualParmSVFGNode>(actualNode);
    if (!actualParm)
      continue;

    const unsigned actualIdx = actualParm->getParamIndex();
    const bool isVarArgExtra =
        callee->isVarArg() && actualIdx >= numFormalParms;

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

  // Get entry function (prefer main, otherwise use first non-declaration)
  const Function *entryFunc = M->getFunction("main");
  if (!entryFunc || entryFunc->isDeclaration()) {
    for (const Function &F : *M) {
      if (!F.isDeclaration()) {
        entryFunc = &F;
        break;
      }
    }
  }
  if (!entryFunc)
    return;

  // Get EntryChi nodes at the entry function
  auto entryChiIt = funcEntryChi.find(entryFunc);
  if (entryChiIt == funcEntryChi.end() || entryChiIt->second.empty())
    return;

  // Build map: memory region -> EntryChi node
  std::unordered_map<uint32_t, SVFGNode *> memRegToEntryChi;
  for (uint32_t chiId : entryChiIt->second) {
    SVFGNode *chiNode = svfg->getNode(chiId);
    if (auto *entryChi = dyn_cast<EntryChiSVFGNode>(chiNode)) {
      memRegToEntryChi[entryChi->getMemReg()] = chiNode;
    }
  }

  // Connect global store nodes to EntryChi nodes
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

    // Connect to EntryChi nodes with intersecting points-to
    for (const auto &pair : memRegToEntryChi) {
      SVFGNode *entryChiNode = pair.second;
      auto *entryChi = dyn_cast<EntryChiSVFGNode>(entryChiNode);
      if (!entryChi)
        continue;

      // Get EntryChi's points-to
      SVFGNodeBS entryChiPts = entryChi->getDefSVFVars();
      if (entryChiPts.empty()) {
        entryChiPts.insert(getOrCreateUnknownObjId());
      }

      // Intersection determines if flow is possible
      SVFGNodeBS intersectPts = intersectPointsToSets(
          storePts, entryChiPts, getOrCreateUnknownObjId());

      // Only connect if there's overlap (or both are unknown)
      if (!intersectPts.empty()) {
        svfg->addEdge(storeNode, entryChiNode, SVFGEdgeK::IntraIndirect,
                      nullptr, intersectPts);
      }
    }
  }
}
