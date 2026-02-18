//===- SVFGBuilderNodes.cpp -- SVFG Node Building Implementation
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
// This file contains node building methods for SVFGBuilder
//
//===----------------------------------------------------------------------===//

#include "IR/SVFG/SVFGBuilder.h"

#include "IR/ICFG/ICFG.h"
#include <llvm/IR/Function.h>
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

void SVFGBuilder::buildNodes() {
  buildTopLevelNodes();
  buildAddressTakenNodes();
  buildFormalParmNodes();
  buildActualParmNodes();
  buildFormalRetNodes();
  buildActualRetNodes();
}

void SVFGBuilder::buildTopLevelNodes() {
  // Use a single NullPtr node for the module (ConstantPointerNull is uniqued).
  uint32_t nullPtrNodeId = std::numeric_limits<uint32_t>::max();

  auto ensureBaseObjIdForValue = [&](const Value *v,
                                     SVFG::ObjectInfo info) -> uint32_t {
    if (!svfg || !v)
      return 0;
    if (const uint32_t existing = svfg->getObjectId(v))
      return existing;

    const uint32_t objId = nextObjId++;
    info.baseObjId = objId;
    svfg->setObjectInfo(objId, info);
    svfg->setObjectValue(objId, v);
    if (const auto *F = dyn_cast<Function>(v))
      svfg->setObjectDebug(objId, ("FUN:" + F->getName()).str());
    else if (const auto *GV = dyn_cast<GlobalValue>(v))
      svfg->setObjectDebug(objId, ("GV:" + GV->getName()).str());
    else if (v->hasName())
      svfg->setObjectDebug(objId, ("OBJ:" + v->getName()).str());
    else
      svfg->setObjectDebug(objId, "OBJ");
    (void)getOrCreateMemRegForObject(objId);
    return objId;
  };

  auto ensureAddrNodeForConstPtr = [&](const Value *v,
                                       IntraBlockNode *at) -> uint32_t {
    if (!v)
      return std::numeric_limits<uint32_t>::max();
    auto it = valueToNode.find(v);
    if (it != valueToNode.end())
      return it->second;
    const bool isConstPtrTarget =
        isa<Function>(v) || isa<GlobalVariable>(v) || isa<GlobalAlias>(v);
    if (!isConstPtrTarget)
      return std::numeric_limits<uint32_t>::max();

    // Also register a base object ID for this constant so DDA can seed points-to
    // sets even when PTA queries return empty (e.g., in minimal IR snippets).
    SVFG::ObjectInfo info;
    info.isFunction = isa<Function>(v);
    info.isGlobal = isa<GlobalValue>(v) && !isa<Function>(v);
    uint32_t baseObjId = ensureBaseObjIdForValue(v, info);

    const uint32_t nodeId = nextNode();
    auto *addrNode = new AddrSVFGNode(nodeId, at, v);
    // Set object ID on AddrSVFGNode (mirrors SVF's getPAGSrcNodeID).
    if (baseObjId != 0)
      addrNode->setObjectId(baseObjId);
    svfg->addNode(addrNode);
    valueToNode.emplace(v, nodeId);
    svfg->setValueNode(v, nodeId);
    return nodeId;
  };
  
  for (auto &pair : *icfg) {
    ICFGNode *node = pair.second;
    IntraBlockNode *blockNode = dyn_cast<IntraBlockNode>(node);
    if (!blockNode)
      continue;

    const BasicBlock *bb = blockNode->getBasicBlock();
    if (!bb)
      continue;

    for (const Instruction &inst : *bb) {
      const bool isStore = isa<StoreInst>(&inst);
      const bool isPhi = isa<PHINode>(&inst);
      const bool isCmp = isa<CmpInst>(&inst);
      const bool isBranch = isa<BranchInst>(&inst);
      const bool isBinary = isa<BinaryOperator>(&inst);
      // UnaryOperator covers fneg; CastInst covers bitcast/trunc/zext/inttoptr
      // etc.  Both can carry pointer-type results, so they must be considered.
      const bool isUnary = isa<UnaryOperator>(&inst) || isa<CastInst>(&inst);
      const bool hasPointerResult = inst.getType()->isPointerTy();
      if (!hasPointerResult && !isStore && !isCmp && !isBranch && !isBinary &&
          !isUnary)
        continue;

      // Create the singleton null node on-demand and map the (uniqued) constant.
      for (const Use &op : inst.operands()) {
        const Value *opVal = op.get();
        if (!isa<ConstantPointerNull>(opVal))
        {
          // Model constant function/global addresses (and their pointer casts)
          // as address nodes so DDA can resolve function pointers and globals.
          if (opVal && opVal->getType()->isPointerTy()) {
            const Value *canon = opVal->stripPointerCasts();
            const uint32_t canonId = ensureAddrNodeForConstPtr(canon, blockNode);
            if (canonId != std::numeric_limits<uint32_t>::max()) {
              valueToNode.emplace(opVal, canonId);
              svfg->setValueNode(opVal, canonId);
            }
          }
          continue;
        }
        if (nullPtrNodeId == std::numeric_limits<uint32_t>::max()) {
          nullPtrNodeId = nextNode();
          auto *nullNode = new NullPtrSVFGNode(nullPtrNodeId, blockNode);
          svfg->addNode(nullNode);
        }
        valueToNode.emplace(opVal, nullPtrNodeId);
        svfg->setValueNode(opVal, nullPtrNodeId);
      }

      if (isa<AllocaInst>(&inst)) {
        uint32_t nodeId = nextNode();
        auto *addrNode = new AddrSVFGNode(nodeId, blockNode, &inst);
        svfg->addNode(addrNode);
        svfg->setDef(&inst, nodeId);
        valueToNode[&inst] = nodeId;
        svfg->setValueNode(&inst, nodeId);
        getOrCreateMemReg(cast<AllocaInst>(&inst));
        SVFG::ObjectInfo info;
        info.isStack = true;
        uint32_t baseObjId = ensureBaseObjIdForValue(&inst, info);
        // Set object ID on AddrSVFGNode (mirrors SVF's getPAGSrcNodeID).
        // Use the base object ID which is also used for edge guard population.
        if (baseObjId != 0)
          addrNode->setObjectId(baseObjId);
        if (config.usePointerAnalysis)
          (void)getObjectIdsForValue(&inst);
      } else if (isHeapAllocation(&inst)) {
        uint32_t nodeId = nextNode();
        auto *addrNode = new AddrSVFGNode(nodeId, blockNode, &inst);
        svfg->addNode(addrNode);
        svfg->setDef(&inst, nodeId);
        valueToNode[&inst] = nodeId;
        svfg->setValueNode(&inst, nodeId);
        (void)getOrCreateMemReg(&inst);
        SVFG::ObjectInfo info;
        info.isHeap = true;
        uint32_t baseObjId = ensureBaseObjIdForValue(&inst, info);
        // Set object ID on AddrSVFGNode (mirrors SVF's getPAGSrcNodeID).
        if (baseObjId != 0)
          addrNode->setObjectId(baseObjId);
        if (config.usePointerAnalysis)
          (void)getObjectIdsForValue(&inst);
      } else if (const LoadInst *load = dyn_cast<LoadInst>(&inst)) {
        auto ptrIt = valueToNode.find(load->getPointerOperand());
        uint32_t ptrNodeId = (ptrIt != valueToNode.end())
                                 ? ptrIt->second
                                 : std::numeric_limits<uint32_t>::max();
        uint32_t nodeId = nextNode();
        auto *loadNode =
            new LoadSVFGNode(nodeId, blockNode, &inst, ptrNodeId);
        svfg->addNode(loadNode);
        svfg->setDef(&inst, nodeId);
        valueToNode[&inst] = nodeId;
        svfg->setValueNode(&inst, nodeId);
        loadToLoadNode[load] = nodeId;
      } else if (const StoreInst *store = dyn_cast<StoreInst>(&inst)) {
        auto ptrIt = valueToNode.find(store->getPointerOperand());
        uint32_t ptrNodeId = (ptrIt != valueToNode.end())
                                 ? ptrIt->second
                                 : std::numeric_limits<uint32_t>::max();
        uint32_t nodeId = nextNode();
        auto *storeNode =
            new StoreSVFGNode(nodeId, blockNode, &inst, ptrNodeId);
        svfg->addNode(storeNode);
        svfg->setDef(&inst, nodeId);
        storeToStoreNode[store] = nodeId;
      } else if (isa<GetElementPtrInst>(&inst)) {
        uint32_t nodeId = nextNode();
        auto *gepNode = new GepSVFGNode(nodeId, blockNode, &inst);
        svfg->addNode(gepNode);
        svfg->setDef(&inst, nodeId);
        valueToNode[&inst] = nodeId;
        svfg->setValueNode(&inst, nodeId);
      } else if (isa<BinaryOperator>(&inst)) {
        uint32_t nodeId = nextNode();
        auto *binaryNode = new BinaryOpSVFGNode(nodeId, blockNode, &inst);
        // Record operand values (matching SVF's OPVers).
        for (unsigned i = 0, e = inst.getNumOperands(); i < e; ++i) {
          binaryNode->setOpVer(i, inst.getOperand(i));
        }
        svfg->addNode(binaryNode);
        svfg->setDef(&inst, nodeId);
        valueToNode[&inst] = nodeId;
        svfg->setValueNode(&inst, nodeId);
      } else if (isa<CmpInst>(&inst)) {
        uint32_t nodeId = nextNode();
        auto *cmpNode = new CmpSVFGNode(nodeId, blockNode, &inst);
        // Record operand values (matching SVF's OPVers).
        for (unsigned i = 0, e = inst.getNumOperands(); i < e; ++i) {
          cmpNode->setOpVer(i, inst.getOperand(i));
        }
        svfg->addNode(cmpNode);
        svfg->setDef(&inst, nodeId);
        valueToNode[&inst] = nodeId;
        svfg->setValueNode(&inst, nodeId);
      } else if (isa<BranchInst>(&inst)) {
        uint32_t nodeId = nextNode();
        auto *branchNode = new BranchSVFGNode(nodeId, blockNode, &inst);
        svfg->addNode(branchNode);
        svfg->setDef(&inst, nodeId);
        valueToNode[&inst] = nodeId;
        svfg->setValueNode(&inst, nodeId);
      } else if (isa<ConstantPointerNull>(&inst)) {
        // Create null pointer node for null constant instructions
        uint32_t nodeId = nextNode();
        auto *nullNode = new NullPtrSVFGNode(nodeId, blockNode);
        svfg->addNode(nullNode);
        svfg->setDef(&inst, nodeId);
        valueToNode[&inst] = nodeId;
        svfg->setValueNode(&inst, nodeId);
      } else if (isPhi) {
        uint32_t nodeId = nextNode();
        const auto *phi = cast<PHINode>(&inst);
        auto *phiNode = new IntraPhiSVFGNode(nodeId, blockNode, phi);
        // Record incoming values as operands (matching SVF's OPVers).
        for (unsigned i = 0, e = phi->getNumIncomingValues(); i < e; ++i) {
          phiNode->setOpVer(i, phi->getIncomingValue(i));
        }
        svfg->addNode(phiNode);
        svfg->setDef(&inst, nodeId);
        valueToNode[&inst] = nodeId;
        svfg->setValueNode(&inst, nodeId);
      } else if (isa<CastInst>(&inst) || isa<UnaryOperator>(&inst)) {
        // Mirrors SVF's UnaryOpVFGNode (bitcast, trunc, zext, sext, fpext,
        // inttoptr, ptrtoint, addrspacecast, fneg …).
        uint32_t nodeId = nextNode();
        auto *unaryNode = new UnaryOpSVFGNode(nodeId, blockNode, &inst);
        // Record single source operand at position 0 (matching SVF's OPVers).
        unaryNode->setOpVer(0, inst.getOperand(0));
        svfg->addNode(unaryNode);
        svfg->setDef(&inst, nodeId);
        valueToNode[&inst] = nodeId;
        svfg->setValueNode(&inst, nodeId);
      } else {
        // Generic copy/move for remaining instructions with pointer results.
        uint32_t nodeId = nextNode();
        auto *copyNode = new CopySVFGNode(nodeId, blockNode, &inst);
        svfg->addNode(copyNode);
        svfg->setDef(&inst, nodeId);
        valueToNode[&inst] = nodeId;
        svfg->setValueNode(&inst, nodeId);
      }
    }
  }
}

void SVFGBuilder::buildAddressTakenNodes() {
  if (!config.buildMSSA)
    return;

  // Handle address-taken variables using AserPTA
  std::unordered_set<const AllocaInst *> processedAllocas;
  for (auto &pair : *icfg) {
    ICFGNode *node = pair.second;
    IntraBlockNode *blockNode = dyn_cast<IntraBlockNode>(node);
    if (!blockNode)
      continue;

    const BasicBlock *bb = blockNode->getBasicBlock();
    if (!bb)
      continue;

    for (const Instruction &inst : *bb) {
      for (const Use &op : inst.operands()) {
        const Value *opVal = op.get();
        if (const AllocaInst *alloca = dyn_cast<AllocaInst>(opVal)) {
          if (!processedAllocas.insert(alloca).second)
            continue;

          bool addressTaken = false;
          for (const Use &use : alloca->uses()) {
            if (!isa<LoadInst>(use.getUser())) {
              addressTaken = true;
              break;
            }
          }

	          if (addressTaken) {
	            std::vector<const void *> ptsVoid = getPointsToSet(alloca);
	            SVFGNodeBS objIds = convertPTAObjectsToObjIDs(ptsVoid);

	            const Function *entryFunc = alloca->getParent()->getParent();

	            const ICFGNode *entryICFGNode = nullptr;
	            if (icfg) {
	              entryICFGNode = const_cast<ICFG *>(icfg)->getIntraBlockNode(
	                  &entryFunc->getEntryBlock());
	            }

	            if (objIds.empty()) {
	              const uint32_t memReg = getOrCreateMemReg(alloca);
	              const uint32_t entryNodeId = nextNode();
	              const uint32_t entryVersion = nextVersion(entryFunc, memReg);
	              auto *entryChi = new EntryChiSVFGNode(
	                  entryNodeId, entryICFGNode, entryFunc, memReg, SVFGNodeBS{},
	                  entryVersion);
	              svfg->addNode(entryChi);
	              svfg->setMSSADef(memReg, entryChi, entryVersion);
	              funcEntryChi[entryFunc].push_back(entryNodeId);
	            } else {
	              for (uint32_t objId : objIds) {
	                const uint32_t memReg = getOrCreateMemRegForObject(objId);
	                const uint32_t entryNodeId = nextNode();
	                const uint32_t entryVersion =
	                    nextVersion(entryFunc, memReg);
	                SVFGNodeBS pts{objId};
	                auto *entryChi = new EntryChiSVFGNode(
	                    entryNodeId, entryICFGNode, entryFunc, memReg, pts,
	                    entryVersion);
	                svfg->addNode(entryChi);
	                svfg->setMSSADef(memReg, entryChi, entryVersion);
	                funcEntryChi[entryFunc].push_back(entryNodeId);
	              }
	            }
	            
	          }
        }
      }
    }
  }

  // Handle globals (both pointer and non-pointer types if address-taken)
  const Module *M = getModuleFromICFG(icfg);
  if (M && config.includeGlobals) {
    for (const GlobalVariable &gv : M->globals()) {
      bool addressTaken = false;
      for (const User *user : gv.users()) {
        if (isa<Instruction>(user)) {
          addressTaken = true;
          break;
        }
        if (const ConstantExpr *ce = dyn_cast<ConstantExpr>(user)) {
          if (ce->getOpcode() == Instruction::GetElementPtr ||
              ce->getOpcode() == Instruction::BitCast ||
              ce->getOpcode() == Instruction::AddrSpaceCast) {
            addressTaken = true;
            break;
          }
        }
      }

      if (addressTaken) {
        std::vector<const void *> ptsVoid = getPointsToSet(&gv);
        SVFGNodeBS objIds = convertPTAObjectsToObjIDs(ptsVoid);

        // Global variable anchoring strategy (mirrors SVF's GlobalBlock approach):
        //
        // 1. Prefer `main` as the anchor (SVF uses a GlobalBlock ICFG node that
        //    feeds into main's entry).
        // 2. If no `main`, collect all functions that DIRECTLY USE the global –
        //    these are plausible entry contexts.  This avoids the unsound choice
        //    of an arbitrary first function in library/multi-entry programs.
        // 3. If the global has no direct instruction users (only ConstantExpr
        //    users), fall back to all non-declaration functions.
        //
        // Note: proper GlobalBlock support requires ICFG changes; this is the
        // best approximation with the current infrastructure.

        // Gather direct-use functions for this global.
        llvm::SmallVector<const Function *, 4> entryFuncs;
        const Function *mainFunc = M->getFunction("main");
        if (mainFunc && !mainFunc->isDeclaration()) {
          entryFuncs.push_back(mainFunc);
        } else {
          // Collect functions that directly use this global.
          for (const User *user : gv.users()) {
            if (const Instruction *userInst = dyn_cast<Instruction>(user)) {
              const Function *F = userInst->getFunction();
              if (F && !F->isDeclaration()) {
                // Deduplicate.
                bool found = false;
                for (const Function *ef : entryFuncs) {
                  if (ef == F) { found = true; break; }
                }
                if (!found) entryFuncs.push_back(F);
              }
            }
          }
          // If no direct users found, fall back to first non-declaration function.
          if (entryFuncs.empty()) {
            for (const Function &F : *M) {
              if (!F.isDeclaration()) {
                entryFuncs.push_back(&F);
                break;
              }
            }
          }
        }

        for (const Function *entryFunc : entryFuncs) {
          const ICFGNode *entryICFGNode = nullptr;
          if (icfg) {
            entryICFGNode = const_cast<ICFG *>(icfg)->getIntraBlockNode(
                &entryFunc->getEntryBlock());
          }
          if (objIds.empty()) {
            const uint32_t memReg = getOrCreateMemReg(&gv);
            // Only create once per (memReg, entryFunc) pair.
            const uint32_t entryNodeId = nextNode();
            const uint32_t entryVersion = nextVersion(entryFunc, memReg);
            auto *entryChi = new EntryChiSVFGNode(
                entryNodeId, entryICFGNode, entryFunc, memReg, SVFGNodeBS{},
                entryVersion);
            svfg->addNode(entryChi);
            svfg->setMSSADef(memReg, entryChi, entryVersion);
            funcEntryChi[entryFunc].push_back(entryNodeId);
          } else {
            for (uint32_t objId : objIds) {
              const uint32_t memReg = getOrCreateMemRegForObject(objId);
              const uint32_t entryNodeId = nextNode();
              const uint32_t entryVersion = nextVersion(entryFunc, memReg);
              SVFGNodeBS pts{objId};
              auto *entryChi = new EntryChiSVFGNode(
                  entryNodeId, entryICFGNode, entryFunc, memReg, pts,
                  entryVersion);
              svfg->addNode(entryChi);
              svfg->setMSSADef(memReg, entryChi, entryVersion);
              funcEntryChi[entryFunc].push_back(entryNodeId);
            }
          }
        }
      }
    }
  }
}

void SVFGBuilder::buildFormalParmNodes() {
  const Module *M = getModuleFromICFG(icfg);
  if (!M)
    return;

  for (const Function &F : *M) {
    if (F.isDeclaration())
      continue;

    unsigned idx = 0;
    for (const auto *argIt = F.arg_begin(); argIt != F.arg_end(); ++argIt, ++idx) {
      const Argument *arg = &*argIt;
      if (!arg->getType()->isPointerTy())
        continue;

      uint32_t nodeId = nextNode();
      auto *formalParm = new FormalParmSVFGNode(nodeId, nullptr, &F, idx);
      svfg->addNode(formalParm);
      svfg->addFormalParm(&F, formalParm);
      valueToNode[arg] = nodeId;
      svfg->setValueNode(arg, nodeId);

      if (config.buildMSSA) {
        std::vector<const void *> ptsVoid = getPointsToSet(arg);
        SVFGNodeBS objIds = convertPTAObjectsToObjIDs(ptsVoid);

        auto &memRegsForArg = argToMemRegs[arg];
        if (objIds.empty()) {
          // Unknown points-to: conservative single region keyed by the argument value.
          const uint32_t memReg = getOrCreateMemReg(arg);
          memRegsForArg.push_back(memReg);
          SVFGNodeBS pts{getOrCreateUnknownObjId()};

          uint32_t formalInNodeId = nextNode();
          auto *formalIn =
              new FormalInSVFGNode(formalInNodeId, nullptr, &F, memReg, pts);
          svfg->addNode(formalIn);
          svfg->addFormalIn(&F, formalIn);

          uint32_t formalOutNodeId = nextNode();
          uint32_t formalOutVersion = nextVersion(&F, memReg);
          auto *formalOut = new FormalOutSVFGNode(
              formalOutNodeId, nullptr, &F, memReg, pts, formalOutVersion);
          svfg->addNode(formalOut);
          svfg->addFormalOut(&F, formalOut);
          svfg->setMSSADef(memReg, formalOut, formalOutVersion);
        } else {
          for (uint32_t objId : objIds) {
            const uint32_t memReg = getOrCreateMemRegForObject(objId);
            memRegsForArg.push_back(memReg);
            SVFGNodeBS pts{objId};

            // Avoid duplicating FormalIn/FormalOut for the same (Function, memReg).
            bool hasFormalIn = false;
            for (SVFGNode *n : svfg->getFormalIns(&F)) {
              if (auto *fi = dyn_cast<FormalInSVFGNode>(n)) {
                if (fi->getMemReg() == memReg) {
                  hasFormalIn = true;
                  break;
                }
              }
            }
            if (!hasFormalIn) {
              uint32_t formalInNodeId = nextNode();
              auto *formalIn =
                  new FormalInSVFGNode(formalInNodeId, nullptr, &F, memReg, pts);
              svfg->addNode(formalIn);
              svfg->addFormalIn(&F, formalIn);
            }

            bool hasFormalOut = false;
            for (SVFGNode *n : svfg->getFormalOuts(&F)) {
              if (auto *fo = dyn_cast<FormalOutSVFGNode>(n)) {
                if (fo->getMemReg() == memReg) {
                  hasFormalOut = true;
                  break;
                }
              }
            }
            if (!hasFormalOut) {
              uint32_t formalOutNodeId = nextNode();
              uint32_t formalOutVersion = nextVersion(&F, memReg);
              auto *formalOut = new FormalOutSVFGNode(
                  formalOutNodeId, nullptr, &F, memReg, pts, formalOutVersion);
              svfg->addNode(formalOut);
              svfg->addFormalOut(&F, formalOut);
              svfg->setMSSADef(memReg, formalOut, formalOutVersion);
            }
          }
        }
      }
    }
  }
}

void SVFGBuilder::buildActualParmNodes() {
  for (auto &pair : *icfg) {
    ICFGNode *node = pair.second;
    IntraBlockNode *blockNode = dyn_cast<IntraBlockNode>(node);
    if (!blockNode)
      continue;

    const BasicBlock *bb = blockNode->getBasicBlock();
    if (!bb)
      continue;

    for (const Instruction &inst : *bb) {
      const CallBase *call = dyn_cast<CallBase>(&inst);
      if (!call)
        continue;

      // Register (funPtrNodeId -> callsites) for SVF-style on-the-fly indirect-call refinement.
      // Use the called operand's SVFG node id as the "funPtr" key.
      if (!call->getCalledFunction()) {
        const Value *calledOp = call->getCalledOperand();
        if (calledOp)
          calledOp = calledOp->stripPointerCasts();
        uint32_t funPtrNodeId = std::numeric_limits<uint32_t>::max();
        if (calledOp) {
          if (SVFGNode *vn = svfg->getValueNode(calledOp)) {
            funPtrNodeId = vn->getId();
          } else if (const auto *ci = dyn_cast<Instruction>(calledOp)) {
            if (SVFGNode *dn = svfg->getDef(ci))
              funPtrNodeId = dn->getId();
          }
        }
        if (funPtrNodeId != std::numeric_limits<uint32_t>::max())
          svfg->addIndCallSite(funPtrNodeId, call);
      }

      // Create ActualParm nodes (one per pointer argument).
      unsigned idx = 0;
      const unsigned numArgs = call->arg_size();
      for (unsigned i = 0; i < numArgs; ++i, ++idx) {
        const Value *argVal = call->getArgOperand(i);
        if (!argVal->getType()->isPointerTy())
          continue;

        uint32_t nodeId = nextNode();
        auto *actualParm =
            new ActualParmSVFGNode(nodeId, blockNode, call, idx);
        svfg->addNode(actualParm);
        svfg->addActualParm(call, actualParm);
        auto argNodeIt = valueToNode.find(argVal);
        if (argNodeIt != valueToNode.end()) {
          if (SVFGNode *argNode = svfg->getNode(argNodeIt->second)) {
            svfg->addEdge(argNode, actualParm, SVFGEdgeK::IntraCopy);
          }
        }
      }

      if (!config.buildMSSA)
        continue;

      // Create ActualIn/ActualOut nodes for all memory regions reachable from pointer arguments.
      std::unordered_set<uint32_t> createdMemRegs;
      for (unsigned i = 0; i < numArgs; ++i) {
        const Value *argVal = call->getArgOperand(i);
        if (!argVal->getType()->isPointerTy())
          continue;

        std::vector<const void *> ptsVoid = getPointsToSet(argVal);
        SVFGNodeBS objIds = convertPTAObjectsToObjIDs(ptsVoid);

        if (objIds.empty()) {
          const uint32_t memReg = getOrCreateMemReg(argVal);
          if (!createdMemRegs.insert(memReg).second)
            continue;

          SVFGNodeBS pts{getOrCreateUnknownObjId()};
          const uint32_t actualInMemId = nextNode();
          auto *actualInMem =
              new ActualInSVFGNode(actualInMemId, blockNode, call, memReg, pts);
          svfg->addNode(actualInMem);
          svfg->addActualIn(call, actualInMem);

          const uint32_t actualOutMemId = nextNode();
          const Function *callerFunc = bb->getParent();
          const uint32_t actualOutVersion = nextVersion(callerFunc, memReg);
          auto *actualOutMem = new ActualOutSVFGNode(
              actualOutMemId, blockNode, call, memReg, pts, actualOutVersion);
          svfg->addNode(actualOutMem);
          svfg->addActualOut(call, actualOutMem);
          svfg->setMSSADef(memReg, actualOutMem, actualOutVersion);
          continue;
        }

        for (uint32_t objId : objIds) {
          const uint32_t memReg = getOrCreateMemRegForObject(objId);
          if (!createdMemRegs.insert(memReg).second)
            continue;
          SVFGNodeBS pts{objId};

          const uint32_t actualInMemId = nextNode();
          auto *actualInMem =
              new ActualInSVFGNode(actualInMemId, blockNode, call, memReg, pts);
          svfg->addNode(actualInMem);
          svfg->addActualIn(call, actualInMem);

          const uint32_t actualOutMemId = nextNode();
          const Function *callerFunc = bb->getParent();
          const uint32_t actualOutVersion = nextVersion(callerFunc, memReg);
          auto *actualOutMem = new ActualOutSVFGNode(
              actualOutMemId, blockNode, call, memReg, pts, actualOutVersion);
          svfg->addNode(actualOutMem);
          svfg->addActualOut(call, actualOutMem);
          svfg->setMSSADef(memReg, actualOutMem, actualOutVersion);
        }
      }
    }
  }
}

void SVFGBuilder::buildFormalRetNodes() {
  const Module *M = getModuleFromICFG(icfg);
  if (!M)
    return;

  for (const Function &F : *M) {
    if (F.isDeclaration())
      continue;

    // Check if function returns a pointer type
    if (!F.getReturnType()->isPointerTy())
      continue;

    // Create formal return node (one per function, not per return statement)
    uint32_t nodeId = nextNode();
    auto *formalRet = new FormalRetSVFGNode(nodeId, nullptr, &F);
    svfg->addNode(formalRet);
    svfg->addFormalRet(&F, formalRet);

    // Connect all return values to the formal return node
    for (const BasicBlock &bb : F) {
      if (const ReturnInst *ret = dyn_cast<ReturnInst>(bb.getTerminator())) {
        const Value *retVal = ret->getReturnValue();
        if (!retVal || !retVal->getType()->isPointerTy())
          continue;

        auto retValIt = valueToNode.find(retVal);
        if (retValIt != valueToNode.end()) {
          SVFGNode *retValNode = svfg->getNode(retValIt->second);
          if (retValNode) {
            svfg->addEdge(retValNode, formalRet, SVFGEdgeK::IntraCopy);
          }
        }
      }
    }
  }
}

void SVFGBuilder::buildActualRetNodes() {
  for (auto &pair : *icfg) {
    ICFGNode *node = pair.second;
    IntraBlockNode *blockNode = dyn_cast<IntraBlockNode>(node);
    if (!blockNode)
      continue;

    const BasicBlock *bb = blockNode->getBasicBlock();
    if (!bb)
      continue;

    for (const Instruction &inst : *bb) {
      const CallBase *call = dyn_cast<CallBase>(&inst);
      if (!call)
        continue;

      // Handle call return value (if call result is used)
      if (call->getType()->isPointerTy()) {
        uint32_t nodeId = nextNode();
        auto *actualRet = new ActualRetSVFGNode(nodeId, blockNode, call);
        svfg->addNode(actualRet);
        svfg->addActualRet(call, actualRet);

        // Bridge interprocedural return flow into the call SSA value.
        auto callValueIt = valueToNode.find(call);
        if (callValueIt != valueToNode.end()) {
          if (SVFGNode *callValueNode = svfg->getNode(callValueIt->second)) {
            svfg->addEdge(actualRet, callValueNode, SVFGEdgeK::IntraCopy);
          }
        }
      }
    }
  }
}
