//===- SVFGBuilder.cpp -- SVFG Builder Implementation
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

#include "IR/SVFG/SVFGBuilder.h"

#include "IR/ICFG/ICFG.h"
#include "IR/SVFG/SVFG.h"
#include "IR/SVFG/SVFGEdge.h"
#include "IR/SVFG/SVFGNode.h"

#include <llvm/ADT/Statistic.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <algorithm>
#include <limits>
#include <map>
#include <queue>
#include <set>

#include "Alias/AserPTA/PointerAnalysis/Context/NoCtx.h"
#include "Alias/AserPTA/PointerAnalysis/Models/LanguageModel/DefaultLangModel/DefaultLangModel.h"
#include "Alias/AserPTA/PointerAnalysis/Models/MemoryModel/FieldSensitive/FSMemModel.h"
#include "Alias/AserPTA/PointerAnalysis/Solver/DeepPropagation.h"

// Define DEBUG_TYPE after including AserPTA headers to avoid redefinition warning
#ifdef DEBUG_TYPE
#undef DEBUG_TYPE
#endif
#define DEBUG_TYPE "SVFGBuilder"
#include "Alias/AserPTA/PointerAnalysis/Solver/PartialUpdateSolver.h"
#include "Alias/AserPTA/PointerAnalysis/Solver/SolverBase.h"
#include "Alias/AserPTA/PointerAnalysis/Solver/WavePropagation.h"
#include <llvm/IR/IntrinsicInst.h>

#undef DEBUG_TYPE

using namespace lotus::analysis;
using namespace llvm;
using namespace aser;

// Type aliases for AserPTA solver configurations
template <typename ctx>
using FSModel = DefaultLangModel<ctx, FSMemModel<ctx>>;

using CIWaveSolver = WavePropagation<FSModel<NoCtx>>;
using CIDeepSolver = DeepPropagation<FSModel<NoCtx>>;
using CIBasicSolver = PartialUpdateSolver<FSModel<NoCtx>>;

// Type alias for Object type
using FSObjectTy = FSObject<NoCtx>;

static SVFGNodeBS intersectPointsToSets(const SVFGNodeBS &lhs,
                                        const SVFGNodeBS &rhs) {
  if (lhs.empty())
    return rhs;
  if (rhs.empty())
    return lhs;

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

// Implement SolverWrapper::destroy()
void SVFGBuilder::SolverWrapper::destroy() {
  if (solver) {
    switch (kind) {
    case SolverKind::Wave:
      delete static_cast<CIWaveSolver*>(solver);
      break;
    case SolverKind::Deep:
      delete static_cast<CIDeepSolver*>(solver);
      break;
    case SolverKind::Basic:
      delete static_cast<CIBasicSolver*>(solver);
      break;
    }
    solver = nullptr;
  }
}

SVFGBuilder::~SVFGBuilder() = default;

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
    if (callEdge->getDstNode() == calleeEntry && callEdge->getCallSite() == call)
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

SVFG *SVFGBuilder::build(const ICFG *icfg) {
  return build(icfg, config);
}

SVFG *SVFGBuilder::build(const ICFG *icfg,
                         const SVFGBuilderConfig &cfg) {
  config = cfg;
  initialize(icfg);

  if (config.usePointerAnalysis) {
    runPointerAnalysis();
  }

  buildNodes();
  buildEdges();

  if (config.buildMSSA) {
    buildMemorySSA();
    buildMemoryPHINodes();
    buildInterproceduralMemoryPHINodes();
    connectMemorySSAEdges();
  }

  buildInterproceduralEdges();

  return svfg.release();
}

void SVFGBuilder::initialize(const ICFG *cfg) {
  icfg = cfg;
  svfg = std::make_unique<SVFG>();
  svfg->setICFG(icfg);
  nextNodeId = 0;
  nextMemRegId = 1;
  // Clean up previous solver wrapper if exists
  ptaSolverWrapper.reset();

  valueToNode.clear();
  allocaToMemReg.clear();
  globalToMemReg.clear();
  heapAllocToMemReg.clear();
  ptrValToMemReg.clear();
  loadToLoadNode.clear();
  storeToStoreNode.clear();
  loadToMuNode.clear();
  storeToChiNode.clear();
  atomicToMu.clear();
  atomicToChi.clear();
  memRegVerToNode.clear();
  funcEntryChi.clear();
  funcExitMu.clear();
  csActualIn.clear();
  csActualOut.clear();
  callToMu.clear();
  callToChi.clear();
  ptaObjectToSVFGNode.clear();
  memRegVersion.clear();
  bbToMemPhi.clear();
  argToMemReg.clear();
  previousPTSets.clear();
}

void SVFGBuilder::runPointerAnalysis() {
  const Module *M = getModuleFromICFG(icfg);
  if (!M)
    return;

  // Create and run AserPTA solver based on configuration
  // Use type-safe wrapper for storage
  switch (config.solverType) {
  case SVFGBuilderConfig::SolverType::Andersen:
  case SVFGBuilderConfig::SolverType::PartialUpdate: {
    auto *solver = new CIBasicSolver();
    solver->analyze(const_cast<Module *>(M));
    ptaSolverWrapper = std::make_unique<SolverWrapper>(
        SolverWrapper::SolverKind::Basic, solver);
    break;
  }
  case SVFGBuilderConfig::SolverType::WavePropagation: {
    auto *solver = new CIWaveSolver();
    solver->analyze(const_cast<Module *>(M));
    ptaSolverWrapper = std::make_unique<SolverWrapper>(
        SolverWrapper::SolverKind::Wave, solver);
    break;
  }
  case SVFGBuilderConfig::SolverType::DeepPropagation: {
    auto *solver = new CIDeepSolver();
    solver->analyze(const_cast<Module *>(M));
    ptaSolverWrapper = std::make_unique<SolverWrapper>(
        SolverWrapper::SolverKind::Deep, solver);
    break;
  }
  default: {
    auto *solver = new CIWaveSolver();
    solver->analyze(const_cast<Module *>(M));
    ptaSolverWrapper = std::make_unique<SolverWrapper>(
        SolverWrapper::SolverKind::Wave, solver);
    break;
  }
  }
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
      const bool hasPointerResult = inst.getType()->isPointerTy();
      if (!hasPointerResult && !isStore && !isCmp && !isBranch && !isBinary)
        continue;

      // Create the singleton null node on-demand and map the (uniqued) constant.
      for (const Use &op : inst.operands()) {
        const Value *opVal = op.get();
        if (!isa<ConstantPointerNull>(opVal))
          continue;
        if (nullPtrNodeId == std::numeric_limits<uint32_t>::max()) {
          nullPtrNodeId = nextNode();
          auto *nullNode = new NullPtrSVFGNode(nullPtrNodeId, blockNode);
          svfg->addNode(nullNode);
        }
        valueToNode.emplace(opVal, nullPtrNodeId);
      }

      if (isa<AllocaInst>(&inst)) {
        uint32_t nodeId = nextNode();
        auto *addrNode = new AddrSVFGNode(nodeId, blockNode, &inst);
        svfg->addNode(addrNode);
        svfg->setDef(&inst, nodeId);
        valueToNode[&inst] = nodeId;
        getOrCreateMemReg(cast<AllocaInst>(&inst));
      } else if (isHeapAllocation(&inst)) {
        uint32_t nodeId = nextNode();
        auto *addrNode = new AddrSVFGNode(nodeId, blockNode, &inst);
        svfg->addNode(addrNode);
        svfg->setDef(&inst, nodeId);
        valueToNode[&inst] = nodeId;
        uint32_t memReg = getOrCreateMemReg(&inst);
        
        // Store PTA object mapping if PTA is available
        if (config.usePointerAnalysis && ptaSolverWrapper && ptaSolverWrapper->solver) {
          std::vector<const void *> ptsVoid = getPointsToSet(&inst);
          for (const void *v : ptsVoid) {
            const FSObjectTy *obj = static_cast<const FSObjectTy *>(v);
            if (obj && obj->getValue() == &inst) {
              ptaObjectToSVFGNode[v] = nodeId;
            }
          }
        }
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
      } else if (isa<BinaryOperator>(&inst)) {
        uint32_t nodeId = nextNode();
        auto *binaryNode = new BinaryOpSVFGNode(nodeId, blockNode, &inst);
        svfg->addNode(binaryNode);
        svfg->setDef(&inst, nodeId);
        valueToNode[&inst] = nodeId;
      } else if (isa<CmpInst>(&inst)) {
        uint32_t nodeId = nextNode();
        auto *cmpNode = new CmpSVFGNode(nodeId, blockNode, &inst);
        svfg->addNode(cmpNode);
        svfg->setDef(&inst, nodeId);
        valueToNode[&inst] = nodeId;
      } else if (isa<BranchInst>(&inst)) {
        uint32_t nodeId = nextNode();
        auto *branchNode = new BranchSVFGNode(nodeId, blockNode, &inst);
        svfg->addNode(branchNode);
        svfg->setDef(&inst, nodeId);
        valueToNode[&inst] = nodeId;
      } else if (isa<ConstantPointerNull>(&inst)) {
        // Create null pointer node for null constant instructions
        uint32_t nodeId = nextNode();
        auto *nullNode = new NullPtrSVFGNode(nodeId, blockNode);
        svfg->addNode(nullNode);
        svfg->setDef(&inst, nodeId);
        valueToNode[&inst] = nodeId;
      } else if (isPhi) {
        uint32_t nodeId = nextNode();
        auto *phiNode =
            new IntraPhiSVFGNode(nodeId, blockNode, cast<PHINode>(&inst));
        svfg->addNode(phiNode);
        svfg->setDef(&inst, nodeId);
        valueToNode[&inst] = nodeId;
      } else {
        uint32_t nodeId = nextNode();
        auto *copyNode = new CopySVFGNode(nodeId, blockNode, &inst);
        svfg->addNode(copyNode);
        svfg->setDef(&inst, nodeId);
        valueToNode[&inst] = nodeId;
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
            uint32_t memReg = getOrCreateMemReg(alloca);
            std::vector<const void *> ptsVoid = getPointsToSet(alloca);
            // Convert PTA objects to SVFG node IDs
            SVFGNodeBS ptsSet = convertPTAObjectsToSVFGNodes(ptsVoid);

            // Create entry chi node
            uint32_t entryNodeId = nextNode();
            const Function *entryFunc = alloca->getParent()->getParent();
            uint32_t entryVersion = memRegVersion[entryFunc][memReg]++;

            auto *entryChi = new EntryChiSVFGNode(
                entryNodeId, blockNode, entryFunc, memReg, ptsSet, entryVersion);
            svfg->addNode(entryChi);
            svfg->setMSSADef(memReg, entryChi, entryVersion);
            funcEntryChi[entryFunc].push_back(entryNodeId);
            
            // Store PTA object mapping for this alloca
            // Reuse ptsVoid from above
            for (const void *v : ptsVoid) {
              const FSObjectTy *obj = static_cast<const FSObjectTy *>(v);
              if (obj && obj->getValue() == alloca) {
                ptaObjectToSVFGNode[v] = entryNodeId;
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
        uint32_t memReg = getOrCreateMemReg(&gv);
        std::vector<const void *> ptsVoid = getPointsToSet(&gv);
        // Convert PTA objects to SVFG node IDs
        SVFGNodeBS ptsSet = convertPTAObjectsToSVFGNodes(ptsVoid);

        // Create global initialization node
        // Find the entry function or create a dummy entry
        const Function *entryFunc = nullptr;
        for (const Function &F : *M) {
          if (!F.isDeclaration() && F.hasName() && 
              (F.getName() == "main" || entryFunc == nullptr)) {
            entryFunc = &F;
            if (F.getName() == "main")
              break;
          }
        }

        if (entryFunc) {
          uint32_t entryNodeId = nextNode();
          uint32_t entryVersion = memRegVersion[entryFunc][memReg]++;
          auto *entryChi = new EntryChiSVFGNode(
              entryNodeId, nullptr, entryFunc, memReg, ptsSet, entryVersion);
          svfg->addNode(entryChi);
          svfg->setMSSADef(memReg, entryChi, entryVersion);
          funcEntryChi[entryFunc].push_back(entryNodeId);
          
          // Also map the global to this EntryChi node for PTA object conversion
          // Store mapping: global -> EntryChi node ID
          ptaObjectToSVFGNode[&gv] = entryNodeId;  // Use global address as key
          for (const void *v : ptsVoid) {
            ptaObjectToSVFGNode[v] = entryNodeId;
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

      if (config.buildMSSA) {
        std::vector<const void *> ptsVoid = getPointsToSet(arg);
        // Convert PTA objects to SVFG node IDs
        SVFGNodeBS ptsSet = convertPTAObjectsToSVFGNodes(ptsVoid);

        // Use unique memory region ID instead of parameter index to avoid collisions
        uint32_t memReg = nextMemRegId++;
        
        // Create FormalIn node (memory region input at function entry)
        uint32_t formalInNodeId = nextNode();
        auto *formalIn =
            new FormalInSVFGNode(formalInNodeId, nullptr, &F, memReg, ptsSet);
        svfg->addNode(formalIn);
        svfg->addFormalIn(&F, formalIn);
        
        // Store PTA object mapping for this argument
        for (const void *v : ptsVoid) {
          const FSObjectTy *obj = static_cast<const FSObjectTy *>(v);
          if (obj && obj->getValue() == arg) {
            ptaObjectToSVFGNode[v] = formalInNodeId;
          }
        }
        
        // Create FormalOut node (memory region output at function exit)
        // This represents memory regions that may be modified by the function
        uint32_t formalOutNodeId = nextNode();
        uint32_t formalOutVersion = memRegVersion[&F][memReg]++;
        auto *formalOut =
            new FormalOutSVFGNode(formalOutNodeId, nullptr, &F, memReg, ptsSet,
                                  formalOutVersion);
        svfg->addNode(formalOut);
        svfg->addFormalOut(&F, formalOut);
        svfg->setMSSADef(memReg, formalOut, formalOutVersion);
        
        // Store mapping from argument to memory region for later use
        // (We'll use this when connecting ActualIn/ActualOut nodes)
        argToMemReg[arg] = memReg;
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

      // Handle both direct and indirect calls
      std::vector<const Function *> callees;
      const Function *directCallee = call->getCalledFunction();
      if (directCallee && !directCallee->isDeclaration()) {
        callees.push_back(directCallee);
      } else if (config.usePointerAnalysis && ptaSolverWrapper && ptaSolverWrapper->solver) {
        // Resolve indirect call targets
        callees = getIndirectCallTargets(call);
        callees = filterCalleesByICFG(icfg, call, callees);
      }
      
      // Handle variadic functions
      bool isVarArg = false;
      if (directCallee && directCallee->getFunctionType()->isVarArg()) {
        isVarArg = true;
      } else if (!callees.empty()) {
        // Check if any callee is variadic
        for (const Function *callee : callees) {
          if (callee->getFunctionType()->isVarArg()) {
            isVarArg = true;
            break;
          }
        }
      }

      // For external functions, create conservative nodes
      if (callees.empty() && directCallee && directCallee->isDeclaration()) {
        // External function - create conservative actual nodes
        unsigned idx = 0;
        for (unsigned i = 0; i < call->arg_size(); ++i, ++idx) {
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
        
        // Create conservative memory nodes if function may modify memory
        if (config.buildMSSA && mayModifyMemory(directCallee)) {
          // Create conservative ActualIn/ActualOut nodes for all pointer arguments
          for (unsigned i = 0; i < call->arg_size(); ++i) {
            const Value *argVal = call->getArgOperand(i);
            if (!argVal->getType()->isPointerTy())
              continue;
            
            // Get points-to set for the argument
            std::vector<const void *> ptsVoid = getPointsToSet(argVal);
            SVFGNodeBS ptsSet = convertPTAObjectsToSVFGNodes(ptsVoid);
            
            // Create a conservative memory region for this external call
            uint32_t memReg = nextMemRegId++;
            
            // Create ActualIn memory node
            uint32_t actualInMemId = nextNode();
            const Function *callerFunc = bb->getParent();
            auto *actualInMem = new ActualInSVFGNode(actualInMemId, blockNode, call, memReg, ptsSet);
            svfg->addNode(actualInMem);
            svfg->addActualIn(call, actualInMem);
            
            // Create ActualOut memory node (external function may modify)
            uint32_t actualOutMemId = nextNode();
            uint32_t actualOutVersion = memRegVersion[callerFunc][memReg]++;
            auto *actualOutMem = new ActualOutSVFGNode(actualOutMemId, blockNode, call, memReg, ptsSet, actualOutVersion);
            svfg->addNode(actualOutMem);
            svfg->addActualOut(call, actualOutMem);
            svfg->setMSSADef(memReg, actualOutMem, actualOutVersion);
          }
        }
        
        continue;
      }

      // First, create ActualParm nodes (one per parameter, shared across all callees)
      // Map: parameter index -> ActualParm node
      std::unordered_map<unsigned, ActualParmSVFGNode *> actualParmNodes;
      unsigned idx = 0;
      unsigned numFixedArgs = call->arg_size();
      
      // Handle fixed arguments
      for (unsigned i = 0; i < numFixedArgs; ++i, ++idx) {
        const Value *argVal = call->getArgOperand(i);
        if (!argVal->getType()->isPointerTy())
          continue;

        uint32_t nodeId = nextNode();
        auto *actualParm =
            new ActualParmSVFGNode(nodeId, blockNode, call, idx);
        svfg->addNode(actualParm);
        svfg->addActualParm(call, actualParm);
        actualParmNodes[idx] = actualParm;
        auto argNodeIt = valueToNode.find(argVal);
        if (argNodeIt != valueToNode.end()) {
          if (SVFGNode *argNode = svfg->getNode(argNodeIt->second)) {
            svfg->addEdge(argNode, actualParm, SVFGEdgeK::IntraCopy);
          }
        }
      }
      
      // Handle variadic arguments conservatively
      // Since we don't know how many variadic args were passed at compile time,
      // we create conservative nodes for all remaining pointer arguments
      if (isVarArg) {
        // For variadic functions, we conservatively assume all remaining arguments
        // after the fixed ones could be pointer arguments
        // In practice, LLVM IR doesn't expose variadic arg count, so we handle
        // this conservatively by ensuring the call site is properly connected
        // Note: Actual variadic arg handling would require runtime information
      }
      
      // Map: (parameter index, memory region) -> ActualIn/ActualOut nodes
      // We create one set of memory nodes per unique memory region across all callees
      std::map<std::pair<unsigned, uint32_t>,
               std::pair<ActualInSVFGNode *, ActualOutSVFGNode *>>
          actualMemNodes;
      
      // Process each possible callee and create connections
      for (const Function *callee : callees) {
        if (callee->isDeclaration())
          continue;

        unsigned idx = 0;
        for (unsigned i = 0; i < call->arg_size(); ++i, ++idx) {
          const Value *argVal = call->getArgOperand(i);
          if (!argVal->getType()->isPointerTy())
            continue;

          // Connect ActualParm → FormalParm (top-level pointer)
          auto actualParmIt = actualParmNodes.find(idx);
          if (actualParmIt != actualParmNodes.end()) {
            ActualParmSVFGNode *actualParm = actualParmIt->second;
            for (auto *formal : svfg->getFormalParms(callee)) {
              if (auto *formalParm = dyn_cast<FormalParmSVFGNode>(formal)) {
                  if (formalParm->getParamIndex() == idx) {
                  svfg->addEdge(actualParm, formalParm, SVFGEdgeK::ParamCall, call);
                  const bool isDirectEdge =
                      (directCallee && directCallee == callee &&
                       !directCallee->isDeclaration());
                  svfg->addEdge(actualParm, formalParm,
                                isDirectEdge ? SVFGEdgeK::CallDir
                                             : SVFGEdgeK::CallInd,
                                call);
                }
              }
            }
          }
          
          // Create ActualIn/ActualOut memory nodes and connect to FormalIn/FormalOut
          if (config.buildMSSA) {
            // Find the corresponding formal parameter argument
            unsigned formalIdx = 0;
            const Argument *formalArg = nullptr;
            for (const auto *argIt = callee->arg_begin(); argIt != callee->arg_end();
                 ++argIt, ++formalIdx) {
              if (formalIdx == idx) {
                formalArg = &*argIt;
                break;
              }
            }
            
            if (formalArg && argToMemReg.count(formalArg)) {
              uint32_t memReg = argToMemReg[formalArg];
              auto memKey = std::make_pair(idx, memReg);
              
              // Create ActualIn/ActualOut nodes if not already created for this parameter/memory region
              ActualInSVFGNode *actualInMem = nullptr;
              ActualOutSVFGNode *actualOutMem = nullptr;
              
              auto memNodeIt = actualMemNodes.find(memKey);
              if (memNodeIt == actualMemNodes.end()) {
                // Get points-to set for the actual argument
                std::vector<const void *> ptsVoid = getPointsToSet(argVal);
                SVFGNodeBS ptsSet = convertPTAObjectsToSVFGNodes(ptsVoid);
                
                // Create ActualIn memory node
                uint32_t actualInMemId = nextNode();
                actualInMem = new ActualInSVFGNode(actualInMemId, blockNode, call, memReg, ptsSet);
                svfg->addNode(actualInMem);
                svfg->addActualIn(call, actualInMem);
                
                // Create ActualOut memory node
                uint32_t actualOutMemId = nextNode();
                const Function *callerFunc = bb->getParent();
                uint32_t actualOutVersion = memRegVersion[callerFunc][memReg]++;
                actualOutMem = new ActualOutSVFGNode(actualOutMemId, blockNode, call, memReg, ptsSet, actualOutVersion);
                svfg->addNode(actualOutMem);
                svfg->addActualOut(call, actualOutMem);
                svfg->setMSSADef(memReg, actualOutMem, actualOutVersion);
                
                actualMemNodes[memKey] = std::make_pair(actualInMem, actualOutMem);
              } else {
                actualInMem = memNodeIt->second.first;
                actualOutMem = memNodeIt->second.second;
              }
              
              // Connect ActualIn → FormalIn
              for (auto *formal : svfg->getFormalIns(callee)) {
                if (auto *formalIn = dyn_cast<FormalInSVFGNode>(formal)) {
                  if (formalIn->getMemReg() == memReg) {
                    if (mayAliasMemoryNodes(actualInMem, formalIn)) {
                      SVFGNodeBS edgePts = intersectPointsToSets(
                          actualInMem->getDefSVFVars(),
                          formalIn->getDefSVFVars());
                      svfg->addEdge(actualInMem, formalIn, SVFGEdgeK::CallAIn,
                                    call, edgePts);
                      svfg->addEdge(actualInMem, formalIn, SVFGEdgeK::CallFIn,
                                    call, edgePts);
                    }
                    break;
                  }
                }
              }
              
              // Connect FormalOut → ActualOut
              for (auto *formal : svfg->getFormalOuts(callee)) {
                if (auto *formalOut = dyn_cast<FormalOutSVFGNode>(formal)) {
                  if (formalOut->getMemReg() == memReg) {
                    if (mayAliasMemoryNodes(formalOut, actualOutMem)) {
                      SVFGNodeBS edgePts = intersectPointsToSets(
                          formalOut->getDefSVFVars(),
                          actualOutMem->getDefSVFVars());
                      svfg->addEdge(formalOut, actualOutMem,
                                    SVFGEdgeK::RetAOut, call, edgePts);
                    }
                    break;
                  }
                }
              }
            }
          }
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

        // Connect to formal returns of all possible callees
        std::vector<const Function *> callees;
        const Function *directCallee = call->getCalledFunction();
        if (directCallee && !directCallee->isDeclaration()) {
          callees.push_back(directCallee);
        } else if (config.usePointerAnalysis && ptaSolverWrapper && ptaSolverWrapper->solver) {
          callees = getIndirectCallTargets(call);
          callees = filterCalleesByICFG(icfg, call, callees);
        }

        for (const Function *callee : callees) {
          if (callee->isDeclaration())
            continue;

          for (auto *formal : svfg->getFormalRets(callee)) {
            if (auto *formalRet = dyn_cast<FormalRetSVFGNode>(formal)) {
              svfg->addEdge(formalRet, actualRet, SVFGEdgeK::ParamRet, call);
              const bool isDirectEdge =
                  (directCallee && directCallee == callee &&
                   !directCallee->isDeclaration());
              svfg->addEdge(formalRet, actualRet,
                            isDirectEdge ? SVFGEdgeK::RetDir
                                         : SVFGEdgeK::RetInd,
                            call);
            }
          }
        }

        // For multi-target indirect calls, materialize an inter-procedural
        // return PHI to merge per-callee formal returns before actual return.
        if (callees.size() > 1) {
          uint32_t phiId = nextNode();
          auto *retPhi = new InterPhiSVFGNode(phiId, blockNode, call);
          svfg->addNode(retPhi);

          for (const Function *callee : callees) {
            if (callee->isDeclaration())
              continue;
            for (auto *formal : svfg->getFormalRets(callee)) {
              if (auto *formalRet = dyn_cast<FormalRetSVFGNode>(formal)) {
                svfg->addEdge(formalRet, retPhi, SVFGEdgeK::ParamRet, call);
              }
            }
          }
          svfg->addEdge(retPhi, actualRet, SVFGEdgeK::IntraPhi);
        }

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

void SVFGBuilder::buildEdges() {
  buildDirectEdges();
  buildCopyEdges();
  buildGepEdges();
  buildPhiEdges();
  buildCmpEdges();
  buildBranchEdges();
  buildMemoryEdges();  // This calls buildLoadEdges() and buildStoreEdges()
}

void SVFGBuilder::buildDirectEdges() {
  for (auto &pair : *icfg) {
    ICFGNode *node = pair.second;
    IntraBlockNode *blockNode = dyn_cast<IntraBlockNode>(node);
    if (!blockNode)
      continue;

    const BasicBlock *bb = blockNode->getBasicBlock();
    if (!bb)
      continue;

    for (const Instruction &inst : *bb) {
      auto dstIt = valueToNode.find(&inst);
      if (dstIt == valueToNode.end())
        continue;

      SVFGNode *dstNode = svfg->getNode(dstIt->second);
      if (!dstNode)
        continue;

      for (const Use &op : inst.operands()) {
        const Value *opVal = op.get();
        auto srcIt = valueToNode.find(opVal);
        if (srcIt == valueToNode.end())
          continue;

        SVFGNode *srcNode = svfg->getNode(srcIt->second);
        if (!srcNode || srcNode == dstNode)
          continue;

        svfg->addEdge(srcNode, dstNode, SVFGEdgeK::IntraDirect);
      }
    }
  }
}

void SVFGBuilder::buildCopyEdges() {
  for (auto &pair : *icfg) {
    ICFGNode *node = pair.second;
    IntraBlockNode *blockNode = dyn_cast<IntraBlockNode>(node);
    if (!blockNode)
      continue;

    const BasicBlock *bb = blockNode->getBasicBlock();
    if (!bb)
      continue;

    for (const Instruction &inst : *bb) {
      auto dstIt = valueToNode.find(&inst);
      if (dstIt == valueToNode.end())
        continue;

      SVFGNode *dstNode = svfg->getNode(dstIt->second);
      if (!dstNode)
        continue;

      for (const Use &op : inst.operands()) {
        const Value *opVal = op.get();
        auto srcIt = valueToNode.find(opVal);
        if (srcIt == valueToNode.end())
          continue;

        SVFGNode *srcNode = svfg->getNode(srcIt->second);
        if (!srcNode)
          continue;

        if (isa<LoadInst>(&inst) || isa<StoreInst>(&inst) ||
            isa<GetElementPtrInst>(&inst) || isa<PHINode>(&inst) ||
            isa<CmpInst>(&inst) || isa<BranchInst>(&inst) ||
            isa<BinaryOperator>(&inst)) {
          continue;
        }

        svfg->addEdge(srcNode, dstNode, SVFGEdgeK::IntraCopy);
      }
    }
  }
}

void SVFGBuilder::buildLoadEdges() {
  auto addConservativeLoadEdges = [&](SVFGNode *dstNode) {
    for (const auto &p : allocaToMemReg) {
      auto nodeIt = valueToNode.find(p.first);
      if (nodeIt != valueToNode.end()) {
        if (SVFGNode *ptNode = svfg->getNode(nodeIt->second))
          svfg->addEdge(ptNode, dstNode, SVFGEdgeK::IntraLoad);
      }
    }
    for (const auto &p : globalToMemReg) {
      auto nodeIt = valueToNode.find(p.first);
      if (nodeIt != valueToNode.end()) {
        if (SVFGNode *ptNode = svfg->getNode(nodeIt->second))
          svfg->addEdge(ptNode, dstNode, SVFGEdgeK::IntraLoad);
      }
    }
    for (const auto &p : heapAllocToMemReg) {
      auto nodeIt = valueToNode.find(p.first);
      if (nodeIt != valueToNode.end()) {
        if (SVFGNode *ptNode = svfg->getNode(nodeIt->second))
          svfg->addEdge(ptNode, dstNode, SVFGEdgeK::IntraLoad);
      }
    }
  };

  for (const auto &pair : loadToLoadNode) {
    const LoadInst *load = pair.first;
    auto dstIt = loadToLoadNode.find(load);
    SVFGNode *dstNode = svfg->getNode(dstIt->second);
    if (!dstNode)
      continue;

    const Value *ptr = load->getPointerOperand();
    auto ptrIt = valueToNode.find(ptr);
    if (ptrIt != valueToNode.end()) {
      if (SVFGNode *ptrNode = svfg->getNode(ptrIt->second)) {
        svfg->addEdge(ptrNode, dstNode, SVFGEdgeK::IntraDirect);
      }
    }

    if (config.usePointerAnalysis && ptaSolverWrapper && ptaSolverWrapper->solver) {
      std::vector<const void *> ptsVoid = getPointsToSet(ptr);
      // Convert PTA objects to SVFG node IDs
      SVFGNodeBS ptsSet = convertPTAObjectsToSVFGNodes(ptsVoid);
      for (uint32_t svfgNodeId : ptsSet) {
        SVFGNode *ptNode = svfg->getNode(svfgNodeId);
        if (ptNode) {
          svfg->addEdge(ptNode, dstNode, SVFGEdgeK::IntraLoad);
        }
      }
      if (ptsSet.empty()) {
        addConservativeLoadEdges(dstNode);
      }
    } else {
      addConservativeLoadEdges(dstNode);
    }
  }
}

void SVFGBuilder::buildStoreEdges() {
  auto addConservativeStoreEdges = [&](SVFGNode *srcNode) {
    for (const auto &p : allocaToMemReg) {
      auto nodeIt = valueToNode.find(p.first);
      if (nodeIt != valueToNode.end()) {
        if (SVFGNode *ptNode = svfg->getNode(nodeIt->second))
          svfg->addEdge(srcNode, ptNode, SVFGEdgeK::IntraStore);
      }
    }
    for (const auto &p : globalToMemReg) {
      auto nodeIt = valueToNode.find(p.first);
      if (nodeIt != valueToNode.end()) {
        if (SVFGNode *ptNode = svfg->getNode(nodeIt->second))
          svfg->addEdge(srcNode, ptNode, SVFGEdgeK::IntraStore);
      }
    }
    for (const auto &p : heapAllocToMemReg) {
      auto nodeIt = valueToNode.find(p.first);
      if (nodeIt != valueToNode.end()) {
        if (SVFGNode *ptNode = svfg->getNode(nodeIt->second))
          svfg->addEdge(srcNode, ptNode, SVFGEdgeK::IntraStore);
      }
    }
  };

  for (const auto &pair : storeToStoreNode) {
    const StoreInst *store = pair.first;
    auto srcIt = storeToStoreNode.find(store);
    SVFGNode *srcNode = svfg->getNode(srcIt->second);
    if (!srcNode)
      continue;

    const Value *ptr = store->getPointerOperand();
    auto ptrIt = valueToNode.find(ptr);
    if (ptrIt != valueToNode.end()) {
      if (SVFGNode *ptrNode = svfg->getNode(ptrIt->second)) {
        svfg->addEdge(ptrNode, srcNode, SVFGEdgeK::IntraDirect);
      }
    }
    const Value *val = store->getValueOperand();
    auto valIt = valueToNode.find(val);
    if (valIt != valueToNode.end()) {
      if (SVFGNode *valNode = svfg->getNode(valIt->second)) {
        svfg->addEdge(valNode, srcNode, SVFGEdgeK::IntraDirect);
      }
    }

    if (config.usePointerAnalysis && ptaSolverWrapper && ptaSolverWrapper->solver) {
      std::vector<const void *> ptsVoid = getPointsToSet(ptr);
      // Convert PTA objects to SVFG node IDs
      SVFGNodeBS ptsSet = convertPTAObjectsToSVFGNodes(ptsVoid);
      for (uint32_t svfgNodeId : ptsSet) {
        SVFGNode *ptNode = svfg->getNode(svfgNodeId);
        if (ptNode) {
          svfg->addEdge(srcNode, ptNode, SVFGEdgeK::IntraStore);
        }
      }
      if (ptsSet.empty()) {
        addConservativeStoreEdges(srcNode);
      }
    } else {
      addConservativeStoreEdges(srcNode);
    }
  }
}

void SVFGBuilder::buildGepEdges() {
  for (auto &pair : *icfg) {
    ICFGNode *node = pair.second;
    IntraBlockNode *blockNode = dyn_cast<IntraBlockNode>(node);
    if (!blockNode)
      continue;

    const BasicBlock *bb = blockNode->getBasicBlock();
    if (!bb)
      continue;

    for (const Instruction &inst : *bb) {
      if (!isa<GetElementPtrInst>(&inst))
        continue;

      auto dstIt = valueToNode.find(&inst);
      if (dstIt == valueToNode.end())
        continue;

      SVFGNode *dstNode = svfg->getNode(dstIt->second);
      if (!dstNode)
        continue;

      if (inst.getNumOperands() == 0)
        continue;
      const Value *ptr = inst.getOperand(0);
      auto srcIt = valueToNode.find(ptr);
      if (srcIt == valueToNode.end())
        continue;

      SVFGNode *srcNode = svfg->getNode(srcIt->second);
      if (srcNode) {
        svfg->addEdge(srcNode, dstNode, SVFGEdgeK::IntraGep);
      }
    }
  }
}

void SVFGBuilder::buildPhiEdges() {
  for (auto &pair : *icfg) {
    ICFGNode *node = pair.second;
    IntraBlockNode *blockNode = dyn_cast<IntraBlockNode>(node);
    if (!blockNode)
      continue;

    const BasicBlock *bb = blockNode->getBasicBlock();
    if (!bb)
      continue;

    for (const PHINode &phi : bb->phis()) {
      auto dstIt = valueToNode.find(&phi);
      if (dstIt == valueToNode.end())
        continue;

      SVFGNode *dstNode = svfg->getNode(dstIt->second);
      if (!dstNode || !dstNode->isPhiNode())
        continue;

      for (unsigned i = 0; i < phi.getNumIncomingValues(); ++i) {
        const Value *incomingVal = phi.getIncomingValue(i);
        auto srcIt = valueToNode.find(incomingVal);
        if (srcIt == valueToNode.end())
          continue;

        SVFGNode *srcNode = svfg->getNode(srcIt->second);
        if (srcNode) {
          svfg->addEdge(srcNode, dstNode, SVFGEdgeK::IntraPhi);
        }
      }
    }
  }
}

void SVFGBuilder::buildCmpEdges() {
  for (auto &pair : *icfg) {
    ICFGNode *node = pair.second;
    IntraBlockNode *blockNode = dyn_cast<IntraBlockNode>(node);
    if (!blockNode)
      continue;
    const BasicBlock *bb = blockNode->getBasicBlock();
    if (!bb)
      continue;

    for (const Instruction &inst : *bb) {
      if (!isa<CmpInst>(&inst))
        continue;

      auto dstIt = valueToNode.find(&inst);
      if (dstIt == valueToNode.end())
        continue;
      SVFGNode *dstNode = svfg->getNode(dstIt->second);
      if (!dstNode)
        continue;

      for (const Use &op : inst.operands()) {
        auto srcIt = valueToNode.find(op.get());
        if (srcIt == valueToNode.end())
          continue;
        SVFGNode *srcNode = svfg->getNode(srcIt->second);
        if (srcNode)
          svfg->addEdge(srcNode, dstNode, SVFGEdgeK::IntraCmp);
      }
    }
  }
}

void SVFGBuilder::buildBranchEdges() {
  for (auto &pair : *icfg) {
    ICFGNode *node = pair.second;
    IntraBlockNode *blockNode = dyn_cast<IntraBlockNode>(node);
    if (!blockNode)
      continue;
    const BasicBlock *bb = blockNode->getBasicBlock();
    if (!bb)
      continue;

    for (const Instruction &inst : *bb) {
      const auto *br = dyn_cast<BranchInst>(&inst);
      if (!br || !br->isConditional())
        continue;

      auto dstIt = valueToNode.find(&inst);
      if (dstIt == valueToNode.end())
        continue;
      SVFGNode *dstNode = svfg->getNode(dstIt->second);
      if (!dstNode)
        continue;

      const Value *cond = br->getCondition();
      auto srcIt = valueToNode.find(cond);
      if (srcIt == valueToNode.end())
        continue;
      SVFGNode *srcNode = svfg->getNode(srcIt->second);
      if (srcNode)
        svfg->addEdge(srcNode, dstNode, SVFGEdgeK::IntraBranch);
    }
  }
}

void SVFGBuilder::buildMemoryEdges() {
  buildLoadEdges();
  buildStoreEdges();
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

          // Get a stable memory region ID for this pointer.
          uint32_t memRegId = getOrCreateMemReg(ptr);

          uint32_t muNodeId = nextNode();
          std::vector<const void *> ptsVoid = getPointsToSet(ptr);
          // Convert PTA objects to SVFG node IDs
          SVFGNodeBS ptsSet = convertPTAObjectsToSVFGNodes(ptsVoid);

          // Find ICFG node for this load
          const ICFGNode *icfgNode = nullptr;
          for (auto &pair : *icfg) {
            if (IntraBlockNode *blockNode = dyn_cast<IntraBlockNode>(pair.second)) {
              if (blockNode->getBasicBlock() == &bb) {
                icfgNode = blockNode;
                break;
              }
            }
          }

          auto *muNode = new LoadMuSVFGNode(muNodeId, icfgNode, load,
                                            memRegId, ptsSet);
          svfg->addNode(muNode);
          loadToMuNode[load] = muNodeId;
        }

        if (const StoreInst *store = dyn_cast<StoreInst>(&inst)) {
          const Value *ptr = store->getPointerOperand();

          if (!isAddressTakenPointer(ptr))
            continue;

          // Get a stable memory region ID for this pointer.
          uint32_t memRegId = getOrCreateMemReg(ptr);
          uint32_t chiVersion = memRegVersion[&F][memRegId]++;

          uint32_t chiNodeId = nextNode();
          std::vector<const void *> ptsVoid = getPointsToSet(ptr);
          // Convert PTA objects to SVFG node IDs
          SVFGNodeBS ptsSet = convertPTAObjectsToSVFGNodes(ptsVoid);

          // Find ICFG node for this store
          const ICFGNode *icfgNode = nullptr;
          for (auto &pair : *icfg) {
            if (IntraBlockNode *blockNode = dyn_cast<IntraBlockNode>(pair.second)) {
              if (blockNode->getBasicBlock() == &bb) {
                icfgNode = blockNode;
                break;
              }
            }
          }

          auto *chiNode = new StoreChiSVFGNode(chiNodeId, icfgNode, store,
                                               memRegId, ptsSet, chiVersion);
          svfg->addNode(chiNode);
          storeToChiNode[store] = chiNodeId;
          svfg->setMSSADef(memRegId, chiNode, chiVersion);
        }

        // Atomic memory ops behave as both load (Mu) and store (Chi).
        if (const auto *rmw = dyn_cast<AtomicRMWInst>(&inst)) {
          const Value *ptr = rmw->getPointerOperand();
          if (isAddressTakenPointer(ptr)) {
            uint32_t memRegId = getOrCreateMemReg(ptr);
            std::vector<const void *> ptsVoid = getPointsToSet(ptr);
            SVFGNodeBS ptsSet = convertPTAObjectsToSVFGNodes(ptsVoid);
            const ICFGNode *icfgNode = findICFGNodeForBlock(icfg, &bb);

            uint32_t muNodeId = nextNode();
            auto *muNode = new LoadMuSVFGNode(muNodeId, icfgNode, nullptr,
                                              memRegId, ptsSet);
            svfg->addNode(muNode);
            atomicToMu[&inst] = muNodeId;

            uint32_t chiNodeId = nextNode();
            uint32_t chiVersion = memRegVersion[&F][memRegId]++;
            auto *chiNode = new StoreChiSVFGNode(chiNodeId, icfgNode, nullptr,
                                                 memRegId, ptsSet, chiVersion);
            svfg->addNode(chiNode);
            atomicToChi[&inst] = chiNodeId;
            svfg->setMSSADef(memRegId, chiNode, chiVersion);
          }
        }
        if (const auto *cmpxchg = dyn_cast<AtomicCmpXchgInst>(&inst)) {
          const Value *ptr = cmpxchg->getPointerOperand();
          if (isAddressTakenPointer(ptr)) {
            uint32_t memRegId = getOrCreateMemReg(ptr);
            std::vector<const void *> ptsVoid = getPointsToSet(ptr);
            SVFGNodeBS ptsSet = convertPTAObjectsToSVFGNodes(ptsVoid);
            const ICFGNode *icfgNode = findICFGNodeForBlock(icfg, &bb);

            uint32_t muNodeId = nextNode();
            auto *muNode = new LoadMuSVFGNode(muNodeId, icfgNode, nullptr,
                                              memRegId, ptsSet);
            svfg->addNode(muNode);
            atomicToMu[&inst] = muNodeId;

            uint32_t chiNodeId = nextNode();
            uint32_t chiVersion = memRegVersion[&F][memRegId]++;
            auto *chiNode = new StoreChiSVFGNode(chiNodeId, icfgNode, nullptr,
                                                 memRegId, ptsSet, chiVersion);
            svfg->addNode(chiNode);
            atomicToChi[&inst] = chiNodeId;
            svfg->setMSSADef(memRegId, chiNode, chiVersion);
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
                  if (IntraBlockNode *blockNode = dyn_cast<IntraBlockNode>(pair.second)) {
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
                    uint32_t memRegId = getOrCreateMemReg(dstPtr);
                    std::vector<const void *> ptsVoid = getPointsToSet(dstPtr);
                    SVFGNodeBS ptsSet = convertPTAObjectsToSVFGNodes(ptsVoid);
                    
                    // Create StoreChi node for the memory modification
                    uint32_t chiNodeId = nextNode();
                    uint32_t chiVersion = memRegVersion[&F][memRegId]++;
                    auto *chiNode = new StoreChiSVFGNode(chiNodeId, icfgNode, 
                                                         nullptr, memRegId,
                                                         ptsSet, chiVersion);
                    svfg->addNode(chiNode);
                    svfg->setMSSADef(memRegId, chiNode, chiVersion);
                    
                    // Also create LoadMu for source (if memcpy/memmove)
                    if (id == Intrinsic::memcpy || id == Intrinsic::memmove) {
                      if (call->arg_size() >= 2) {
                        const Value *srcPtr = call->getArgOperand(1);
                        if (srcPtr->getType()->isPointerTy()) {
                          uint32_t srcMemRegId = getOrCreateMemReg(srcPtr);
                          std::vector<const void *> srcPtsVoid = getPointsToSet(srcPtr);
                          SVFGNodeBS srcPtsSet = convertPTAObjectsToSVFGNodes(srcPtsVoid);
                          
                          uint32_t muNodeId = nextNode();
                          auto *muNode = new LoadMuSVFGNode(muNodeId, icfgNode,
                                                            nullptr, srcMemRegId, srcPtsSet);
                          svfg->addNode(muNode);
                          
                          // Connect mu -> chi to represent the copy
                          svfg->addEdge(muNode, chiNode, SVFGEdgeK::IntraCopy);
                        }
                      }
                    }
                  }
                }
                continue;  // Skip normal CallMu/CallChi creation
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

          // Create CallMu node (memory use at call site)
          uint32_t callMuId = nextNode();
          SVFGNodeBS ptsSet;
          
          // Get points-to set of all pointer arguments
          for (unsigned i = 0; i < call->arg_size(); ++i) {
            const Value *arg = call->getArgOperand(i);
            if (!arg->getType()->isPointerTy())
              continue;
            
            std::vector<const void *> ptsVoid = getPointsToSet(arg);
            // Convert PTA objects to SVFG node IDs and merge into ptsSet
            SVFGNodeBS argPtsSet = convertPTAObjectsToSVFGNodes(ptsVoid);
            ptsSet.insert(argPtsSet.begin(), argPtsSet.end());
          }

          // Find ICFG node
          const ICFGNode *icfgNode = nullptr;
          for (auto &pair : *icfg) {
            if (IntraBlockNode *blockNode = dyn_cast<IntraBlockNode>(pair.second)) {
              if (blockNode->getBasicBlock() == &bb) {
                icfgNode = blockNode;
                break;
              }
            }
          }

          uint32_t callMemReg = nextMemRegId++;
          auto *callMu = new CallMuSVFGNode(callMuId, icfgNode, call, callMemReg, ptsSet);
          svfg->addNode(callMu);
          callToMu[call] = callMuId;

          // Create CallChi node (memory def at call site)
          uint32_t callChiId = nextNode();
          uint32_t callChiVersion = memRegVersion[&F][callMemReg]++;
          auto *callChi =
              new CallChiSVFGNode(callChiId, icfgNode, call, callMemReg, ptsSet,
                                  callChiVersion);
          svfg->addNode(callChi);
          callToChi[call] = callChiId;
          svfg->setMSSADef(callMemReg, callChi, callChiVersion);
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
  // This handles all cases: multiple predecessors, single predecessor with PHI, etc.
  for (const Function &F : *M) {
    if (F.isDeclaration())
      continue;

    // Track which memory regions have defs in each basic block
    std::unordered_map<const BasicBlock *, std::set<uint32_t>> bbToMemRegs;
    
    // First pass: collect memory regions with defs in each block
    for (const BasicBlock &bb : F) {
      for (const Instruction &inst : bb) {
        if (const StoreInst *store = dyn_cast<StoreInst>(&inst)) {
          auto chiIt = storeToChiNode.find(store);
          if (chiIt != storeToChiNode.end()) {
            SVFGNode *chiNode = svfg->getNode(chiIt->second);
            if (auto *chi = dyn_cast<StoreChiSVFGNode>(chiNode)) {
              uint32_t memReg = chi->getMemReg();
              bbToMemRegs[&bb].insert(memReg);
            }
          }
        }
        auto atomicChiIt = atomicToChi.find(&inst);
        if (atomicChiIt != atomicToChi.end()) {
          SVFGNode *chiNode = svfg->getNode(atomicChiIt->second);
          if (auto *chi = dyn_cast<StoreChiSVFGNode>(chiNode)) {
            uint32_t memReg = chi->getMemReg();
            bbToMemRegs[&bb].insert(memReg);
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
    // 2. At least two predecessors have different defs (or one has def, one has PHI)
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
          std::set<SVFGNode*> incomingDefs;
          
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
                  auto chiIt = storeToChiNode.find(store);
                  if (chiIt != storeToChiNode.end()) {
                    SVFGNode *chiNode = svfg->getNode(chiIt->second);
                    if (auto *chi = dyn_cast<StoreChiSVFGNode>(chiNode)) {
                      if (chi->getMemReg() == memReg) {
                        incomingDef = chiNode;
                        // Keep the last one (most recent def)
                      }
                    }
                  }
                }
                auto atomicChiIt = atomicToChi.find(&inst);
                if (atomicChiIt != atomicToChi.end()) {
                  SVFGNode *chiNode = svfg->getNode(atomicChiIt->second);
                  if (auto *chi = dyn_cast<StoreChiSVFGNode>(chiNode)) {
                    if (chi->getMemReg() == memReg) {
                      incomingDef = chiNode;
                    }
                  }
                }
              }
              
              // If no def found in predecessor, check EntryChi (if pred is entry block)
              if (!incomingDef && pred == &F.getEntryBlock()) {
                for (uint32_t entryChiId : funcEntryChi[&F]) {
                  SVFGNode *entryChiNode = svfg->getNode(entryChiId);
                  if (auto *entryChi = dyn_cast<EntryChiSVFGNode>(entryChiNode)) {
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
          
          // Need PHI if we have multiple different incoming defs
          // OR if we have a single predecessor with a PHI (to maintain SSA form)
          if (incomingDefs.size() > 1) {
            needsPhi = true;
          } else if (numPreds == 1 && incomingDefs.size() == 1) {
            // Check if the single predecessor has a PHI - if so, we need one too
            const BasicBlock *singlePred = *pred_begin(&bb);
            auto predPhiIt = bbToMemPhi[singlePred].find(memReg);
            if (predPhiIt != bbToMemPhi[singlePred].end()) {
              needsPhi = true;
            }
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
                    auto chiIt = storeToChiNode.find(store);
                    if (chiIt != storeToChiNode.end()) {
                      SVFGNode *chiNode = svfg->getNode(chiIt->second);
                      if (auto *chi = dyn_cast<StoreChiSVFGNode>(chiNode)) {
                        if (chi->getMemReg() == memReg) {
                          ptsSet = chi->getDefSVFVars();
                          break;
                        }
                      }
                    }
                  }
                  auto atomicChiIt = atomicToChi.find(&inst);
                  if (atomicChiIt != atomicToChi.end()) {
                    SVFGNode *chiNode = svfg->getNode(atomicChiIt->second);
                    if (auto *chi = dyn_cast<StoreChiSVFGNode>(chiNode)) {
                      if (chi->getMemReg() == memReg) {
                        ptsSet = chi->getDefSVFVars();
                        break;
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
            uint32_t version = memRegVersion[&F][memReg]++;
            
            // Find ICFG node for this basic block
            const ICFGNode *icfgNode = nullptr;
            for (auto &pair : *icfg) {
              if (IntraBlockNode *blockNode = dyn_cast<IntraBlockNode>(pair.second)) {
                if (blockNode->getBasicBlock() == &bb) {
                  icfgNode = blockNode;
                  break;
                }
              }
            }

            auto *phiNode = new IntraMSSAPhiSVFGNode(phiNodeId, icfgNode, memReg, version, ptsSet);
            svfg->addNode(phiNode);
            bbToMemPhi[&bb][memReg] = phiNodeId;
            changed = true;
            
            // Connect PHI to incoming defs from predecessors
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
	                    auto chiIt = storeToChiNode.find(store);
	                    if (chiIt != storeToChiNode.end()) {
	                      SVFGNode *chiNode = svfg->getNode(chiIt->second);
	                      if (auto *chi = dyn_cast<StoreChiSVFGNode>(chiNode)) {
	                        if (chi->getMemReg() == memReg) {
	                          lastChi = chiNode;
	                        }
	                      }
	                    }
	                  }
                  auto atomicChiIt = atomicToChi.find(&inst);
                  if (atomicChiIt != atomicToChi.end()) {
                    SVFGNode *chiNode = svfg->getNode(atomicChiIt->second);
                    if (auto *chi = dyn_cast<StoreChiSVFGNode>(chiNode)) {
                      if (chi->getMemReg() == memReg) {
                        lastChi = chiNode;
                      }
                    }
                  }
                }
                incomingDef = lastChi;
                
                // If no def found in predecessor, check EntryChi (if pred is entry block)
                if (!incomingDef && pred == &F.getEntryBlock()) {
                  for (uint32_t entryChiId : funcEntryChi[&F]) {
                    SVFGNode *entryChiNode = svfg->getNode(entryChiId);
                    if (auto *entryChi = dyn_cast<EntryChiSVFGNode>(entryChiNode)) {
                      if (entryChi->getMemReg() == memReg) {
                        incomingDef = entryChiNode;
                        break;
                      }
                    }
                  }
                }
              }
              
              // Connect incoming def to PHI (use EntryChi as fallback if no def found)
              if (incomingDef) {
                svfg->addEdge(incomingDef, phiNode, SVFGEdgeK::IntraPhi);
              } else {
                // No def found - connect to EntryChi if available (conservative)
                for (uint32_t entryChiId : funcEntryChi[&F]) {
                  SVFGNode *entryChiNode = svfg->getNode(entryChiId);
                  if (auto *entryChi = dyn_cast<EntryChiSVFGNode>(entryChiNode)) {
                    if (entryChi->getMemReg() == memReg) {
                      svfg->addEdge(entryChiNode, phiNode, SVFGEdgeK::IntraPhi);
                      break;
                    }
                  }
                }
              }
            }
          }
        }
      }
    }
  }
}

void SVFGBuilder::buildInterproceduralMemoryPHINodes() {
  if (!config.buildMSSA)
    return;

  const Module *M = getModuleFromICFG(icfg);
  if (!M)
    return;

  // Create inter-procedural memory PHI nodes for memory regions that flow
  // across call boundaries. These are needed when:
  // 1. A memory region is modified in a callee and flows back to caller
  // 2. A memory region flows from caller to callee and back
  
  for (const Function &F : *M) {
    if (F.isDeclaration())
      continue;

    // For each memory region that has FormalIn/FormalOut nodes,
    // check if we need inter-procedural PHI nodes
    
    // Collect memory regions that have both FormalIn and FormalOut
    std::set<uint32_t> interprocMemRegs;
    for (auto *formalIn : svfg->getFormalIns(&F)) {
      if (auto *formalInMem = dyn_cast<FormalInSVFGNode>(formalIn)) {
        uint32_t memReg = formalInMem->getMemReg();
        // Check if there's a corresponding FormalOut
        for (auto *formalOut : svfg->getFormalOuts(&F)) {
          if (auto *formalOutMem = dyn_cast<FormalOutSVFGNode>(formalOut)) {
            if (formalOutMem->getMemReg() == memReg) {
              interprocMemRegs.insert(memReg);
              break;
            }
          }
        }
      }
    }

    // Create InterMSSAPhiSVFGNode for formal parameter memory flow
    for (uint32_t memReg : interprocMemRegs) {
      // Find FormalIn and FormalOut nodes for this memory region
      FormalInSVFGNode *formalIn = nullptr;
      FormalOutSVFGNode *formalOut = nullptr;
      
      for (auto *formal : svfg->getFormalIns(&F)) {
        if (auto *fi = dyn_cast<FormalInSVFGNode>(formal)) {
          if (fi->getMemReg() == memReg) {
            formalIn = fi;
            break;
          }
        }
      }
      
      for (auto *formal : svfg->getFormalOuts(&F)) {
        if (auto *fo = dyn_cast<FormalOutSVFGNode>(formal)) {
          if (fo->getMemReg() == memReg) {
            formalOut = fo;
            break;
          }
        }
      }

      if (!formalIn || !formalOut)
        continue;

      // Get points-to set from FormalIn
      SVFGNodeBS ptsSet = formalIn->getDefSVFVars();
      
      // Create InterMSSAPhiSVFGNode for formal parameter (function entry)
      uint32_t formalPhiId = nextNode();
      const ICFGNode *entryICFGNode = nullptr;
      // Find entry ICFG node
      for (auto &pair : *icfg) {
        if (IntraBlockNode *blockNode = dyn_cast<IntraBlockNode>(pair.second)) {
          if (blockNode->getBasicBlock() == &F.getEntryBlock()) {
            entryICFGNode = blockNode;
            break;
          }
        }
      }
      
      auto *formalPhi = new InterMSSAPhiSVFGNode(
          formalPhiId, entryICFGNode, &F, memReg, ptsSet);
      svfg->addNode(formalPhi);
      
      // Connect FormalIn -> InterPhi -> FormalOut
      svfg->addEdge(formalIn, formalPhi, SVFGEdgeK::IntraPhi);
      svfg->addEdge(formalPhi, formalOut, SVFGEdgeK::IntraPhi);
    }

    // Now handle call sites: create InterMSSAPhiSVFGNode for actual return memory flow
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

        // Get all ActualIn and ActualOut nodes for this call
        std::set<uint32_t> callMemRegs;
        for (auto *actualIn : svfg->getActualIns(call)) {
          if (auto *ai = dyn_cast<ActualInSVFGNode>(actualIn)) {
            uint32_t memReg = ai->getMemReg();
            // Check if there's a corresponding ActualOut
            for (auto *actualOut : svfg->getActualOuts(call)) {
              if (auto *ao = dyn_cast<ActualOutSVFGNode>(actualOut)) {
                if (ao->getMemReg() == memReg) {
                  callMemRegs.insert(memReg);
                  break;
                }
              }
            }
          }
        }

        // Create InterMSSAPhiSVFGNode for actual return memory flow
        for (uint32_t memReg : callMemRegs) {
          ActualInSVFGNode *actualIn = nullptr;
          ActualOutSVFGNode *actualOut = nullptr;
          
          for (auto *actual : svfg->getActualIns(call)) {
            if (auto *ai = dyn_cast<ActualInSVFGNode>(actual)) {
              if (ai->getMemReg() == memReg) {
                actualIn = ai;
                break;
              }
            }
          }
          
          for (auto *actual : svfg->getActualOuts(call)) {
            if (auto *ao = dyn_cast<ActualOutSVFGNode>(actual)) {
              if (ao->getMemReg() == memReg) {
                actualOut = ao;
                break;
              }
            }
          }

          if (!actualIn || !actualOut)
            continue;

          // Get points-to set from ActualIn
          SVFGNodeBS ptsSet = actualIn->getDefSVFVars();
          
          // Create InterMSSAPhiSVFGNode for actual return (call site)
          uint32_t actualPhiId = nextNode();
          auto *actualPhi = new InterMSSAPhiSVFGNode(
              actualPhiId, blockNode, call, memReg, ptsSet);
          svfg->addNode(actualPhi);
          
          // Connect ActualIn -> InterPhi -> ActualOut
          svfg->addEdge(actualIn, actualPhi, SVFGEdgeK::IntraPhi);
          svfg->addEdge(actualPhi, actualOut, SVFGEdgeK::IntraPhi);
        }
      }
    }
  }
}

void SVFGBuilder::connectMemorySSAEdges() {
  if (!config.buildMSSA)
    return;

  const Module *M = getModuleFromICFG(icfg);
  if (!M)
    return;

  // Map: memory region -> last def (StoreChi/EntryChi) in each basic block
  std::unordered_map<const BasicBlock*, std::unordered_map<uint32_t, SVFGNode*>> lastDefInBlock;
  
  // Map: memory region -> EntryChi node for function entry
  std::unordered_map<const Function*, std::unordered_map<uint32_t, SVFGNode*>> funcEntryChiMap;

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
          auto chiIt = storeToChiNode.find(store);
          if (chiIt != storeToChiNode.end()) {
            SVFGNode *chiNode = svfg->getNode(chiIt->second);
            if (auto *chi = dyn_cast<StoreChiSVFGNode>(chiNode)) {
              uint32_t memReg = chi->getMemReg();
              lastDefInBlock[&bb][memReg] = chiNode;
            }
          }
        }
      }
    }
  }

  // Second pass: connect LoadMu nodes to their reaching defs
  // Track last def per memory region per function (needed for fourth pass)
  std::unordered_map<const Function*, std::unordered_map<const BasicBlock*, std::unordered_map<uint32_t, SVFGNode*>>> lastDefAtBlockMap;
  
  for (const Function &F : *M) {
    if (F.isDeclaration())
      continue;

    // Track last def per memory region as we traverse blocks
    // Use a worklist-based approach to handle control flow properly
    std::unordered_map<const BasicBlock*, std::unordered_map<uint32_t, SVFGNode*>> &lastDefAtBlock = lastDefAtBlockMap[&F];
    
    // Initialize entry block with EntryChi nodes
    for (auto &pair : funcEntryChiMap[&F]) {
      lastDefAtBlock[&F.getEntryBlock()][pair.first] = pair.second;
    }
    
    // Worklist fixpoint over CFG blocks to correctly propagate backedges.
    std::queue<const BasicBlock*> worklist;
    std::set<const BasicBlock*> inQueue;
    worklist.push(&F.getEntryBlock());
    inQueue.insert(&F.getEntryBlock());
    
    // Process blocks using worklist (handles control flow properly)
    while (!worklist.empty()) {
      const BasicBlock *bb = worklist.front();
      worklist.pop();
      inQueue.erase(bb);
      
      // Get last defs at entry of this block
      std::unordered_map<uint32_t, SVFGNode*> lastDef;
      
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
              
              // If multiple predecessors have defs for same region, use PHI if exists
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
                  // Multiple predecessors have different defs and no PHI: create
                  // one on demand to avoid losing reaching definitions.
                  uint32_t phiId = createMemoryPHI(memReg, bb);
                  SVFGNode *phiNode = svfg->getNode(phiId);
                  if (phiNode) {
                    svfg->addEdge(existingDefIt->second, phiNode,
                                  SVFGEdgeK::IntraPhi);
                    svfg->addEdge(def, phiNode, SVFGEdgeK::IntraPhi);
                    lastDef[memReg] = phiNode;
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
          auto muIt = loadToMuNode.find(load);
          if (muIt != loadToMuNode.end()) {
            SVFGNode *muNode = svfg->getNode(muIt->second);
            if (auto *mu = dyn_cast<LoadMuSVFGNode>(muNode)) {
              uint32_t memReg = mu->getMemReg();
              
              // Find reaching def for this memory region
              SVFGNode *reachingDef = nullptr;
              
              // Check if there's a PHI node for this memory region at block entry
              auto phiIt = bbToMemPhi[bb].find(memReg);
              if (phiIt != bbToMemPhi[bb].end()) {
                // Use PHI node as reaching def
                reachingDef = svfg->getNode(phiIt->second);
              } else {
                // Use last def for this memory region
                auto defIt = lastDef.find(memReg);
                if (defIt != lastDef.end() && defIt->second) {
                  reachingDef = defIt->second;
                } else {
                  // No def found, connect to EntryChi if available
                  auto entryIt = funcEntryChiMap[&F].find(memReg);
                  if (entryIt != funcEntryChiMap[&F].end()) {
                    reachingDef = entryIt->second;
                  }
                }
              }
              
              if (reachingDef) {
                // Connect def -> mu (IntraMu edge)
                svfg->addEdge(reachingDef, muNode, SVFGEdgeK::IntraMu);
                if (isa<EntryChiSVFGNode>(reachingDef)) {
                  svfg->addEdge(reachingDef, muNode, SVFGEdgeK::EntryChi);
                }
              }
            }
          }
        }
        auto atomicMuIt = atomicToMu.find(&inst);
        if (atomicMuIt != atomicToMu.end()) {
          SVFGNode *muNode = svfg->getNode(atomicMuIt->second);
          if (auto *mu = dyn_cast<LoadMuSVFGNode>(muNode)) {
            uint32_t memReg = mu->getMemReg();
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
              svfg->addEdge(reachingDef, muNode, SVFGEdgeK::IntraMu);
              if (isa<EntryChiSVFGNode>(reachingDef)) {
                svfg->addEdge(reachingDef, muNode, SVFGEdgeK::EntryChi);
              }
            }
          }
        }

        if (const StoreInst *store = dyn_cast<StoreInst>(&inst)) {
          auto chiIt = storeToChiNode.find(store);
          if (chiIt != storeToChiNode.end()) {
            SVFGNode *chiNode = svfg->getNode(chiIt->second);
            if (auto *chi = dyn_cast<StoreChiSVFGNode>(chiNode)) {
              uint32_t memReg = chi->getMemReg();
              auto prevIt = lastDef.find(memReg);
              if (prevIt != lastDef.end() && prevIt->second && prevIt->second != chiNode) {
                svfg->addEdge(prevIt->second, chiNode, SVFGEdgeK::IntraChi);
              }
              
              // Update last def for this memory region
              lastDef[memReg] = chiNode;
              
              // Note: Version increment happens when creating StoreChi nodes,
              // not here. This is a use site, not a def creation site.
            }
          }
        }
        auto atomicChiIt = atomicToChi.find(&inst);
        if (atomicChiIt != atomicToChi.end()) {
          SVFGNode *chiNode = svfg->getNode(atomicChiIt->second);
          if (auto *chi = dyn_cast<StoreChiSVFGNode>(chiNode)) {
            uint32_t memReg = chi->getMemReg();
            auto prevIt = lastDef.find(memReg);
            if (prevIt != lastDef.end() && prevIt->second && prevIt->second != chiNode) {
              svfg->addEdge(prevIt->second, chiNode, SVFGEdgeK::IntraChi);
            }
            lastDef[memReg] = chiNode;
          }
        }

        if (const auto *call = dyn_cast<CallBase>(&inst)) {
          auto muIt = callToMu.find(call);
          auto chiIt = callToChi.find(call);
          if (muIt != callToMu.end() && chiIt != callToChi.end()) {
            SVFGNode *callMuNode = svfg->getNode(muIt->second);
            SVFGNode *callChiNode = svfg->getNode(chiIt->second);
            if (callMuNode && callChiNode) {
              for (auto &memDef : lastDef) {
                if (memDef.second) {
                  svfg->addEdge(memDef.second, callMuNode, SVFGEdgeK::CallMu);
                }
              }
              svfg->addEdge(callMuNode, callChiNode, SVFGEdgeK::CallChi);
              for (auto &memDef : lastDef) {
                memDef.second = callChiNode;
              }
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
      std::unordered_map<uint32_t, SVFGNode*> lastChiInBlock;
      
      for (const Instruction &inst : bb) {
        // First, connect any LoadMu to preceding StoreChi
        if (const LoadInst *load = dyn_cast<LoadInst>(&inst)) {
          auto muIt = loadToMuNode.find(load);
          if (muIt != loadToMuNode.end()) {
            SVFGNode *muNode = svfg->getNode(muIt->second);
            if (auto *mu = dyn_cast<LoadMuSVFGNode>(muNode)) {
              uint32_t memReg = mu->getMemReg();
              auto chiIt = lastChiInBlock.find(memReg);
              if (chiIt != lastChiInBlock.end()) {
                // Connect chi -> mu
                svfg->addEdge(chiIt->second, muNode, SVFGEdgeK::IntraMu);
              }
            }
          }
        }
        auto atomicMuIt = atomicToMu.find(&inst);
        if (atomicMuIt != atomicToMu.end()) {
          SVFGNode *muNode = svfg->getNode(atomicMuIt->second);
          if (auto *mu = dyn_cast<LoadMuSVFGNode>(muNode)) {
            uint32_t memReg = mu->getMemReg();
            auto chiIt = lastChiInBlock.find(memReg);
            if (chiIt != lastChiInBlock.end()) {
              svfg->addEdge(chiIt->second, muNode, SVFGEdgeK::IntraMu);
            }
          }
        }

        // Then, update lastChiInBlock for StoreChi
        if (const StoreInst *store = dyn_cast<StoreInst>(&inst)) {
          auto chiIt = storeToChiNode.find(store);
          if (chiIt != storeToChiNode.end()) {
            SVFGNode *chiNode = svfg->getNode(chiIt->second);
            if (auto *chi = dyn_cast<StoreChiSVFGNode>(chiNode)) {
              uint32_t memReg = chi->getMemReg();
              lastChiInBlock[memReg] = chiNode;
            }
          }
        }
        auto atomicChiIt = atomicToChi.find(&inst);
        if (atomicChiIt != atomicToChi.end()) {
          SVFGNode *chiNode = svfg->getNode(atomicChiIt->second);
          if (auto *chi = dyn_cast<StoreChiSVFGNode>(chiNode)) {
            uint32_t memReg = chi->getMemReg();
            lastChiInBlock[memReg] = chiNode;
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
    std::vector<const BasicBlock*> returnBlocks;
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
        
        // If no def found in return blocks, check all blocks for the most recent def
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
        uint32_t retMuVersion = memRegVersion[&F][memReg]++;
        SVFGNodeBS ptsSet = formalOutMem->getDefSVFVars();
        auto *retMu =
            new RetMuSVFGNode(retMuId, nullptr, &F, memReg, ptsSet, retMuVersion);
        svfg->addNode(retMu);
        funcExitMu[&F].push_back(retMuId);
        svfg->setMSSADef(memReg, retMu, retMuVersion);

        if (lastDef) {
          svfg->addEdge(lastDef, retMu, SVFGEdgeK::RetMu);
        } else {
          // If no def found, connect EntryChi (if exists) to indicate
          // the memory region is passed through without modification.
          auto entryIt = funcEntryChiMap.find(&F);
          if (entryIt != funcEntryChiMap.end()) {
            auto memEntryIt = entryIt->second.find(memReg);
            if (memEntryIt != entryIt->second.end()) {
              svfg->addEdge(memEntryIt->second, retMu, SVFGEdgeK::RetMu);
            }
          }
        }
        svfg->addEdge(retMu, formalOutMem, SVFGEdgeK::RetFOut);
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
                 ptaSolverWrapper->solver) {
        callees = getIndirectCallTargets(call);
        callees = filterCalleesByICFG(icfg, call, callees);
      }

      for (SVFGNode *actualNode : svfg->getActualParms(call)) {
        auto *actualParm = dyn_cast<ActualParmSVFGNode>(actualNode);
        if (!actualParm)
          continue;
        for (const Function *callee : callees) {
          for (SVFGNode *formal : svfg->getFormalParms(callee)) {
            auto *formalParm = dyn_cast<FormalParmSVFGNode>(formal);
            if (!formalParm)
              continue;
            if (formalParm->getParamIndex() != actualParm->getParamIndex())
              continue;
            svfg->addEdge(actualParm, formalParm, SVFGEdgeK::ParamCall, call);
            const bool isDirectEdge =
                (directCallee && directCallee == callee &&
                 !directCallee->isDeclaration());
            svfg->addEdge(actualParm, formalParm,
                          isDirectEdge ? SVFGEdgeK::CallDir : SVFGEdgeK::CallInd,
                          call);
          }
        }
      }

      for (SVFGNode *actualNode : svfg->getActualIns(call)) {
        auto *actualIn = dyn_cast<ActualInSVFGNode>(actualNode);
        if (!actualIn)
          continue;
        for (const Function *callee : callees) {
          for (SVFGNode *formal : svfg->getFormalIns(callee)) {
            auto *formalIn = dyn_cast<FormalInSVFGNode>(formal);
            if (formalIn && formalIn->getMemReg() == actualIn->getMemReg()) {
              if (mayAliasMemoryNodes(actualIn, formalIn)) {
                SVFGNodeBS edgePts = intersectPointsToSets(
                    actualIn->getDefSVFVars(), formalIn->getDefSVFVars());
                svfg->addEdge(actualIn, formalIn, SVFGEdgeK::CallAIn, call,
                              edgePts);
                svfg->addEdge(actualIn, formalIn, SVFGEdgeK::CallFIn, call,
                              edgePts);
              }
            }
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
                 ptaSolverWrapper->solver) {
        callees = getIndirectCallTargets(call);
        callees = filterCalleesByICFG(icfg, call, callees);
      }

      for (SVFGNode *actualNode : svfg->getActualRets(call)) {
        auto *actualRet = dyn_cast<ActualRetSVFGNode>(actualNode);
        if (!actualRet)
          continue;
        for (const Function *callee : callees) {
          for (SVFGNode *formal : svfg->getFormalRets(callee)) {
            auto *formalRet = dyn_cast<FormalRetSVFGNode>(formal);
            if (!formalRet)
              continue;
            svfg->addEdge(formalRet, actualRet, SVFGEdgeK::ParamRet, call);
            const bool isDirectEdge =
                (directCallee && directCallee == callee &&
                 !directCallee->isDeclaration());
            svfg->addEdge(formalRet, actualRet,
                          isDirectEdge ? SVFGEdgeK::RetDir : SVFGEdgeK::RetInd,
                          call);
          }
        }
      }

      for (SVFGNode *actualNode : svfg->getActualOuts(call)) {
        auto *actualOut = dyn_cast<ActualOutSVFGNode>(actualNode);
        if (!actualOut)
          continue;
        for (const Function *callee : callees) {
          for (SVFGNode *formal : svfg->getFormalOuts(callee)) {
            auto *formalOut = dyn_cast<FormalOutSVFGNode>(formal);
            if (formalOut && formalOut->getMemReg() == actualOut->getMemReg()) {
              if (mayAliasMemoryNodes(formalOut, actualOut)) {
                SVFGNodeBS edgePts = intersectPointsToSets(
                    formalOut->getDefSVFVars(), actualOut->getDefSVFVars());
                svfg->addEdge(formalOut, actualOut, SVFGEdgeK::RetAOut, call,
                              edgePts);
              }
            }
          }
        }
      }
    }
  }
}

uint32_t SVFGBuilder::getOrCreateNode(const Value *val) {
  auto it = valueToNode.find(val);
  if (it != valueToNode.end()) {
    return it->second;
  }

  uint32_t id = nextNode();
  valueToNode[val] = id;
  return id;
}

uint32_t SVFGBuilder::getOrCreateMemReg(const AllocaInst *alloca) {
  auto it = allocaToMemReg.find(alloca);
  if (it != allocaToMemReg.end()) {
    return it->second;
  }

  uint32_t memRegId = nextMemRegId++;
  allocaToMemReg[alloca] = memRegId;
  return memRegId;
}

uint32_t SVFGBuilder::getOrCreateMemReg(const GlobalVariable *global) {
  auto it = globalToMemReg.find(global);
  if (it != globalToMemReg.end()) {
    return it->second;
  }

  uint32_t memRegId = nextMemRegId++;
  globalToMemReg[global] = memRegId;
  return memRegId;
}

uint32_t SVFGBuilder::getOrCreateMemReg(const Instruction *heapAlloc) {
  auto it = heapAllocToMemReg.find(heapAlloc);
  if (it != heapAllocToMemReg.end()) {
    return it->second;
  }

  uint32_t memRegId = nextMemRegId++;
  heapAllocToMemReg[heapAlloc] = memRegId;
  return memRegId;
}

uint32_t SVFGBuilder::getOrCreateMemReg(const Value *ptrVal) {
  if (!ptrVal)
    return 0;

  if (const auto *alloca = dyn_cast<AllocaInst>(ptrVal)) {
    return getOrCreateMemReg(alloca);
  }

  if (const auto *gv = dyn_cast<GlobalVariable>(ptrVal)) {
    return getOrCreateMemReg(gv);
  }

  if (const auto *arg = dyn_cast<Argument>(ptrVal)) {
    auto argIt = argToMemReg.find(arg);
    if (argIt != argToMemReg.end()) {
      return argIt->second;
    }
  }

  if (const auto *inst = dyn_cast<Instruction>(ptrVal)) {
    if (isHeapAllocation(inst)) {
      return getOrCreateMemReg(inst);
    }
  }

  auto it = ptrValToMemReg.find(ptrVal);
  if (it != ptrValToMemReg.end()) {
    return it->second;
  }

  const uint32_t memRegId = nextMemRegId++;
  ptrValToMemReg[ptrVal] = memRegId;
  return memRegId;
}

SVFGNodeBS SVFGBuilder::convertPTAObjectsToSVFGNodes(
    const std::vector<const void *> &ptaObjects) {
  SVFGNodeBS result;

  for (const void *v : ptaObjects) {
    const FSObjectTy *obj = static_cast<const FSObjectTy *>(v);
    if (!obj)
      continue;

    // Check if we've already mapped this PTA object to an SVFG node
    auto cachedIt = ptaObjectToSVFGNode.find(v);
    if (cachedIt != ptaObjectToSVFGNode.end()) {
      result.insert(cachedIt->second);
      continue;
    }

    // Get the allocation site value from the PTA object
    const Value *allocSite = obj->getValue();
    if (!allocSite)
      continue;

    // Try to find the corresponding SVFG node
    uint32_t svfgNodeId = 0;
    bool found = false;

    // Check if it's an alloca
    if (const AllocaInst *alloca = dyn_cast<AllocaInst>(allocSite)) {
      // First check for Addr node
      auto nodeIt = valueToNode.find(alloca);
      if (nodeIt != valueToNode.end()) {
        svfgNodeId = nodeIt->second;
        found = true;
      } else {
        // Check for EntryChi node if address-taken
        auto memRegIt = allocaToMemReg.find(alloca);
        if (memRegIt != allocaToMemReg.end()) {
          uint32_t memReg = memRegIt->second;
          const Function *F = alloca->getParent()->getParent();
          for (uint32_t entryChiId : funcEntryChi[F]) {
            SVFGNode *entryChiNode = svfg->getNode(entryChiId);
            if (auto *entryChi = dyn_cast<EntryChiSVFGNode>(entryChiNode)) {
              if (entryChi->getMemReg() == memReg) {
                svfgNodeId = entryChiId;
                found = true;
                break;
              }
            }
          }
        }
      }
    }
    // Check if it's a heap allocation
    else if (const Instruction *inst = dyn_cast<Instruction>(allocSite)) {
      if (isHeapAllocation(inst)) {
        // First check for Addr node
        auto nodeIt = valueToNode.find(inst);
        if (nodeIt != valueToNode.end()) {
          svfgNodeId = nodeIt->second;
          found = true;
        } else {
          // Check for memory region mapping
          auto memRegIt = heapAllocToMemReg.find(inst);
          if (memRegIt != heapAllocToMemReg.end()) {
            // Fallback: find any existing node that represents this memory region.
            const uint32_t memReg = memRegIt->second;
            for (auto &svfgPair : *svfg) {
              SVFGNode *candidate = svfgPair.second;
              if (candidate && candidate->getMemReg() == memReg) {
                svfgNodeId = candidate->getId();
                found = true;
                break;
              }
            }
          }
        }
      } else {
        // Other instruction types - check generic pointer value mapping
        auto memRegIt = ptrValToMemReg.find(inst);
        if (memRegIt != ptrValToMemReg.end()) {
          // Try to find corresponding node
          auto nodeIt = valueToNode.find(inst);
          if (nodeIt != valueToNode.end()) {
            svfgNodeId = nodeIt->second;
            found = true;
          }
        }
      }
    }
    // Check if it's a global variable
    else if (const GlobalVariable *global = dyn_cast<GlobalVariable>(allocSite)) {
      // First check if there's an Addr node
      auto nodeIt = valueToNode.find(global);
      if (nodeIt != valueToNode.end()) {
        svfgNodeId = nodeIt->second;
        found = true;
      } else {
        // Look for EntryChi node for this global using memory region mapping
        auto globalMemRegIt = globalToMemReg.find(global);
        if (globalMemRegIt != globalToMemReg.end()) {
          uint32_t memReg = globalMemRegIt->second;
          // Search through EntryChi nodes more efficiently
          const Module *M = getModuleFromICFG(icfg);
          if (M) {
            // Try to find EntryChi node by memory region
            // We need to find which function has the EntryChi for this global
            // Typically, globals are initialized in main or entry function
            for (const Function &F : *M) {
              if (F.isDeclaration())
                continue;
              
              for (uint32_t entryChiId : funcEntryChi[&F]) {
                SVFGNode *entryChiNode = svfg->getNode(entryChiId);
                if (auto *entryChi = dyn_cast<EntryChiSVFGNode>(entryChiNode)) {
                  if (entryChi->getMemReg() == memReg) {
                    svfgNodeId = entryChiId;
                    found = true;
                    break;
                  }
                }
              }
              if (found)
                break;
            }
          }
        }
      }
    }
    // Check if it's a function argument
    else if (const Argument *arg = dyn_cast<Argument>(allocSite)) {
      // Arguments have memory regions mapped via argToMemReg
      auto argMemRegIt = argToMemReg.find(arg);
      if (argMemRegIt != argToMemReg.end()) {
        uint32_t memReg = argMemRegIt->second;
        const Function *F = arg->getParent();
        // Find FormalIn node for this argument
        for (auto *formal : svfg->getFormalIns(F)) {
          if (auto *formalIn = dyn_cast<FormalInSVFGNode>(formal)) {
            if (formalIn->getMemReg() == memReg) {
              svfgNodeId = formalIn->getId();
              found = true;
              break;
            }
          }
        }
      }
    }

    if (found) {
      // Cache the mapping for future use
      ptaObjectToSVFGNode[v] = svfgNodeId;
      result.insert(svfgNodeId);
    } else {
      // If not found, create a conservative Dummy node to ensure we don't lose edges
      // This marks the edge as imprecise but maintains soundness
      uint32_t dummyId = nextNode();
      auto *dummy = new DummySVFGNode(dummyId, nullptr);
      svfg->addNode(dummy);
      ptaObjectToSVFGNode[v] = dummyId;
      result.insert(dummyId);
    }
  }

  return result;
}

std::vector<const void *>
SVFGBuilder::getPointsToSet(const Value *ptr) {
  std::vector<const FSObjectTy *> ptsResult;
  std::vector<const void *> result;

  if (!config.usePointerAnalysis || !ptaSolverWrapper || !ptaSolverWrapper->solver) {
    // Conservative fallback: return all known memory regions
    // This is a sound but imprecise approximation
    for (auto &p : allocaToMemReg) {
      // Note: We can't create actual FSObject pointers here without PTA
      // So we return empty set, which will be handled conservatively by callers
    }
    for (auto &p : heapAllocToMemReg) {
      // Same for heap allocations
    }
    return result;
  }

  // Use AserPTA to get points-to set
  // Call getPointsTo based on solver kind
  bool found = false;
  
  switch (ptaSolverWrapper->kind) {
  case SolverWrapper::SolverKind::Wave: {
    auto *solver = static_cast<CIWaveSolver*>(ptaSolverWrapper->solver);
    if (solver) {
      solver->getPointsTo(nullptr, ptr, ptsResult);
      found = true;
    }
    break;
  }
  case SolverWrapper::SolverKind::Deep: {
    auto *solver = static_cast<CIDeepSolver*>(ptaSolverWrapper->solver);
    if (solver) {
      solver->getPointsTo(nullptr, ptr, ptsResult);
      found = true;
    }
    break;
  }
  case SolverWrapper::SolverKind::Basic: {
    auto *solver = static_cast<CIBasicSolver*>(ptaSolverWrapper->solver);
    if (solver) {
      solver->getPointsTo(nullptr, ptr, ptsResult);
      found = true;
    }
    break;
  }
  }

  // Convert to void* vector for opaque interface
  if (found) {
    for (const auto *obj : ptsResult) {
      result.push_back(static_cast<const void*>(obj));
    }
  }

  return result;
}

std::vector<const Function *>
SVFGBuilder::getIndirectCallTargets(const CallBase *call) {
  std::vector<const Function *> targets;
  
  if (!config.usePointerAnalysis || !ptaSolverWrapper || !ptaSolverWrapper->solver)
    return targets;

  const Value *calledVal = call->getCalledOperand();
  if (!calledVal || !calledVal->getType()->isPointerTy())
    return targets;

  // Get points-to set of the called value
  std::vector<const void *> ptsVoid = getPointsToSet(calledVal);
  std::vector<const FSObjectTy *> pts;
  pts.reserve(ptsVoid.size());
  for (const void *v : ptsVoid) {
    pts.push_back(static_cast<const FSObjectTy*>(v));
  }

  // Filter for Function pointers
  // In AserPTA, function objects have getValue() that returns the Function*
  for (const FSObjectTy *obj : pts) {
    if (!obj)
      continue;
    
    // Get the allocation site value (for functions, this is the Function*)
    const Value *val = obj->getValue();
    if (!val)
      continue;
    
    // Check if it's a function
    if (const Function *F = dyn_cast<Function>(val)) {
      // Avoid duplicates
      if (std::find(targets.begin(), targets.end(), F) == targets.end()) {
        targets.push_back(F);
      }
    }
    
    // Also check if the value type is a function pointer
    if (val->getType()->isPointerTy()) {
      if (const FunctionType *FTy = dyn_cast<FunctionType>(val->getType()->getPointerElementType())) {
        // This is a function pointer type, but we need the actual function
        // Try to find it by checking if val itself is a function after stripping casts
        if (const Function *F = dyn_cast<Function>(val->stripPointerCasts())) {
          if (std::find(targets.begin(), targets.end(), F) == targets.end()) {
            targets.push_back(F);
          }
        }
      }
    }
  }
  
  // Enhanced handling: function pointers in structs/arrays
  // If calledVal is a load from a struct field or array element, we need to
  // handle field-sensitive points-to analysis
  if (const LoadInst *load = dyn_cast<LoadInst>(calledVal)) {
    const Value *srcPtr = load->getPointerOperand();
    // Get points-to set of the source pointer (struct/array)
    std::vector<const void *> srcPtsVoid = getPointsToSet(srcPtr);
    for (const void *v : srcPtsVoid) {
      const FSObjectTy *srcObj = static_cast<const FSObjectTy *>(v);
      if (!srcObj)
        continue;
      
      // For field-sensitive analysis, the object might represent a struct field
      // containing a function pointer. Try to get the function from the object.
      const Value *srcVal = srcObj->getValue();
      if (srcVal && srcVal->getType()->isPointerTy()) {
        // Check if this points to a function pointer stored in a struct
        // This is a conservative approximation - full field-sensitive handling
        // would require tracking struct field offsets
        if (const Function *F = dyn_cast<Function>(srcVal->stripPointerCasts())) {
          if (std::find(targets.begin(), targets.end(), F) == targets.end()) {
            targets.push_back(F);
          }
        }
      }
    }
  }

  // Fallback: check if called value directly points to a function
  // (handles direct function pointers that weren't captured in PTA)
  if (targets.empty()) {
    if (const Function *F = dyn_cast<Function>(calledVal->stripPointerCasts())) {
      targets.push_back(F);
    }
  }

  return targets;
}

bool SVFGBuilder::isHeapAllocation(const Instruction *inst) const {
  if (!inst)
    return false;

  if (const CallBase *call = dyn_cast<CallBase>(inst)) {
    const Function *callee = call->getCalledFunction();
    if (!callee)
      return false;

    StringRef name = callee->getName();
    // Check for common heap allocation functions
    return name == "malloc" || name == "calloc" || name == "realloc" ||
           name == "aligned_alloc" || name == "valloc" || name == "memalign" ||
           name == "posix_memalign" || name == "_Znwm" || name == "_Znam" || // new/new[]
           name == "__builtin_malloc" || name == "__builtin_calloc";
  }

  return false;
}

bool SVFGBuilder::isAddressTakenPointer(const Value *ptr) const {
  if (!ptr || !ptr->getType()->isPointerTy())
    return false;

  const Value *base = ptr->stripPointerCasts();
  if (isa<AllocaInst>(base) || isa<GlobalVariable>(base))
    return true;
  if (const auto *arg = dyn_cast<Argument>(base))
    return argToMemReg.find(arg) != argToMemReg.end();
  if (const auto *inst = dyn_cast<Instruction>(base)) {
    if (isHeapAllocation(inst))
      return true;
  }

  if (!config.usePointerAnalysis || !ptaSolverWrapper || !ptaSolverWrapper->solver)
    return false;

  // PTA can identify memory-backed pointers even when base is a cast/gep/load.
  return !const_cast<SVFGBuilder *>(this)->getPointsToSet(ptr).empty();
}

bool SVFGBuilder::mayAliasMemoryNodes(const MSSASVFGNode *lhs,
                                      const MSSASVFGNode *rhs) const {
  if (!lhs || !rhs)
    return false;

  const SVFGNodeBS lhsPts = lhs->getDefSVFVars();
  const SVFGNodeBS rhsPts = rhs->getDefSVFVars();

  // Conservative: if either set is unknown/empty, keep the edge.
  if (lhsPts.empty() || rhsPts.empty())
    return true;

  for (uint32_t id : lhsPts) {
    if (rhsPts.count(id))
      return true;
  }
  return false;
}

uint32_t SVFGBuilder::createMemRegVerNode(uint32_t memReg, uint32_t version,
                                          const ICFGNode *icfgNode) {
  MemRegVer mrv{memReg, version};
  auto it = memRegVerToNode.find(mrv);
  if (it != memRegVerToNode.end()) {
    return it->second;
  }

  uint32_t nodeId = nextNode();
  SVFGNodeBS pts;

  // Get points-to information from the memory region
  // Find the alloca or heap allocation that created this region
  const AllocaInst *alloca = nullptr;
  for (auto &p : allocaToMemReg) {
    if (p.second == memReg) {
      alloca = p.first;
      break;
    }
  }

  if (alloca) {
    std::vector<const void *> ptsVoid = getPointsToSet(alloca);
    // Convert PTA objects to SVFG node IDs
    pts = convertPTAObjectsToSVFGNodes(ptsVoid);
  }

  auto *phiNode =
      new IntraMSSAPhiSVFGNode(nodeId, icfgNode, memReg, version, pts);
  svfg->addNode(phiNode);
  memRegVerToNode[mrv] = nodeId;

  return nodeId;
}

uint32_t SVFGBuilder::createMemoryPHI(uint32_t memReg, const BasicBlock *bb) {
  // Create a memory PHI node at a control flow merge point
  // This is called when multiple definitions of a memory region reach a basic block
  
  // Check if PHI already exists for this memory region at this block
  auto phiIt = bbToMemPhi[bb].find(memReg);
  if (phiIt != bbToMemPhi[bb].end()) {
    return phiIt->second;
  }
  
  uint32_t nodeId = nextNode();
  const Function *F = bb->getParent();
  // Use per-function versioning to avoid collisions across functions
  uint32_t version = memRegVersion[F][memReg]++;
  SVFGNodeBS pts;

  // Get points-to set from EntryChi or StoreChi nodes
  // Try to find a def for this memory region to get points-to info
  for (uint32_t entryChiId : funcEntryChi[F]) {
    SVFGNode *entryChiNode = svfg->getNode(entryChiId);
    if (auto *entryChi = dyn_cast<EntryChiSVFGNode>(entryChiNode)) {
      if (entryChi->getMemReg() == memReg) {
        pts = entryChi->getDefSVFVars();
        break;
      }
    }
  }

  // Find ICFG node for this basic block
  const ICFGNode *icfgNode = nullptr;
  for (auto &pair : *icfg) {
    if (IntraBlockNode *blockNode = dyn_cast<IntraBlockNode>(pair.second)) {
      if (blockNode->getBasicBlock() == bb) {
        icfgNode = blockNode;
        break;
      }
    }
  }

  auto *phiNode = new IntraMSSAPhiSVFGNode(nodeId, icfgNode, memReg, version, pts);
  svfg->addNode(phiNode);
  bbToMemPhi[bb][memReg] = nodeId;
  
  return nodeId;
}

bool SVFGBuilder::updateSVFG(SVFG *existingSVFG) {
  if (!existingSVFG || !icfg) {
    return false;
  }

  // For semantic fidelity we rebuild the graph and atomically swap contents.
  std::unique_ptr<SVFG> rebuilt(build(icfg, config));
  if (!rebuilt) {
    return false;
  }
  existingSVFG->swapWith(*rebuilt);
  return true;
}

void SVFGBuilder::updateMemorySSAEdges(SVFG *svfg) {
  if (!svfg || !config.buildMSSA) {
    return;
  }

  // Keep update behavior semantically equivalent to the full builder.
  (void)updateSVFG(svfg);
}

bool SVFGBuilder::mayModifyMemory(const Function *F) {
  std::unordered_set<const Function *> visited;
  return mayModifyMemory(F, visited);
}

bool SVFGBuilder::mayModifyMemory(
    const Function *F, std::unordered_set<const Function *> &visited) {
  if (!F)
    return true;  // Conservative: assume unknown functions modify memory
  if (!visited.insert(F).second) {
    // Break recursion cycles conservatively.
    return true;
  }
  
  // External/declaration functions: check LLVM attributes
  if (F->isDeclaration()) {
    // Check LLVM function attributes for memory behavior
    if (F->onlyReadsMemory()) {
      return false;  // Known to only read memory
    }
    if (F->doesNotAccessMemory()) {
      return false;  // Known to not access memory
    }
    // Conservative: assume external functions may modify memory
    // unless explicitly marked otherwise
    return true;
  }
  
  // For defined functions, check if they have any store instructions
  // or calls that might modify memory
  for (const BasicBlock &bb : *F) {
    for (const Instruction &inst : bb) {
      // Check for store instructions
      if (isa<StoreInst>(&inst)) {
        return true;
      }
      
      // Check for atomic operations that modify memory
      if (isa<AtomicRMWInst>(&inst) || isa<AtomicCmpXchgInst>(&inst)) {
        return true;
      }
      
      // Check for calls that might modify memory
      if (const CallBase *call = dyn_cast<CallBase>(&inst)) {
        // Skip LLVM intrinsics that don't modify memory
        if (const IntrinsicInst *intrinsic = dyn_cast<IntrinsicInst>(call)) {
          Intrinsic::ID id = intrinsic->getIntrinsicID();
          switch (id) {
            case Intrinsic::dbg_value:
            case Intrinsic::dbg_declare:
            case Intrinsic::dbg_label:
            case Intrinsic::lifetime_start:
            case Intrinsic::lifetime_end:
            case Intrinsic::invariant_start:
            case Intrinsic::invariant_end:
              continue;  // These don't modify memory
            default:
              // Other intrinsics might modify memory
              return true;
          }
        }
        
        const Function *callee = call->getCalledFunction();
        if (!callee) {
          // Indirect call - conservative: assume it might modify
          return true;
        }
        
        if (callee->isDeclaration()) {
          // External function - check attributes
          if (callee->onlyReadsMemory() || callee->doesNotAccessMemory()) {
            continue;  // Known to not modify memory
          }
          // Conservative: assume it might modify
          return true;
        }
        
        // Recursive check: if callee modifies memory, so does caller
        if (mayModifyMemory(callee, visited)) {
          return true;
        }
      }
    }
  }
  
  return false;
}
