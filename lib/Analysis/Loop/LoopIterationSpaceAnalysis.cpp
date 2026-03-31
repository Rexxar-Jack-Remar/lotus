/*
 * Copyright 2026 Lotus contributors
 */
#include "Analysis/Loop/LoopIterationSpaceAnalysis.h"

#include "llvm/Analysis/Delinearization.h"

namespace lotus {
namespace analysis {
namespace loop {

LoopIterationSpaceAnalysis::LoopIterationSpaceAnalysis(
    LoopTree *loops, InductionVariableManager &ivManager,
    llvm::ScalarEvolution &SE)
    : loops{loops}, ivManager{ivManager} {
  indexIVInstructionSCEVs(SE);
  if (this->ivInstructionsBySCEV.empty()) {
    return;
  }

  computeMemoryAccessSpace(SE);
  identifyIVForMemoryAccessSubscripts(SE);
  identifyNonOverlappingAccessesBetweenIterationsAcrossOneLoopInvocation(SE);
}

bool LoopIterationSpaceAnalysis::
    areInstructionsAccessingDisjointMemoryLocationsBetweenIterations(
        Instruction *I, Instruction *J) const {
  if ((!I) || (!J) ||
      (this->accessSpaceByInstruction.find(I) ==
       this->accessSpaceByInstruction.end()) ||
      (this->accessSpaceByInstruction.find(J) ==
       this->accessSpaceByInstruction.end())) {
    return false;
  }

  auto *accessSpaceI = this->accessSpaceByInstruction.at(I);
  auto *accessSpaceJ = this->accessSpaceByInstruction.at(J);
  return this->areMemoryAccessSpaceNotOverlappingOrExactlyTheSame(accessSpaceI,
                                                                  accessSpaceJ);
}

bool LoopIterationSpaceAnalysis::
    areMemoryAccessSpaceNotOverlappingOrExactlyTheSame(
        MemoryAccessSpace *accessSpaceI,
        MemoryAccessSpace *accessSpaceJ) const {
  if (!accessSpaceI->isAnalyzed || !accessSpaceJ->isAnalyzed) {
    return false;
  }
  if (this->nonOverlappingAccessesBetweenIterations.count(accessSpaceI) == 0 ||
      this->nonOverlappingAccessesBetweenIterations.count(accessSpaceJ) == 0) {
    return false;
  }
  if (accessSpaceI == accessSpaceJ) {
    return true;
  }
  if (this->spacesThatCannotOverlap.find(accessSpaceI) ==
          this->spacesThatCannotOverlap.end() ||
      this->spacesThatCannotOverlap.find(accessSpaceJ) ==
          this->spacesThatCannotOverlap.end()) {
    return false;
  }
  auto &notOverlapSetForI = this->spacesThatCannotOverlap.at(accessSpaceI);
  auto &notOverlapSetForJ = this->spacesThatCannotOverlap.at(accessSpaceJ);
  return notOverlapSetForI.count(accessSpaceJ) != 0 ||
         notOverlapSetForJ.count(accessSpaceI) != 0;
}

bool LoopIterationSpaceAnalysis::
    analyzeToCheckIfMemoryAccessSpaceNotOverlappingOrExactlyTheSame(
        MemoryAccessSpace *accessSpaceI,
        MemoryAccessSpace *accessSpaceJ) const {
  if (this->nonOverlappingAccessesBetweenIterations.count(accessSpaceI) == 0 ||
      this->nonOverlappingAccessesBetweenIterations.count(accessSpaceJ) == 0) {
    return false;
  }

  if (accessSpaceI->memoryAccessorBasePointerSCEV !=
      accessSpaceJ->memoryAccessorBasePointerSCEV) {
    return false;
  }
  if (accessSpaceI == accessSpaceJ) {
    return true;
  }

  return this->isMemoryAccessSpaceEquivalentForTopLoopIVSubscript(accessSpaceI,
                                                                  accessSpaceJ);
}

bool LoopIterationSpaceAnalysis::
    isMemoryAccessSpaceEquivalentForTopLoopIVSubscript(
        MemoryAccessSpace *space1, MemoryAccessSpace *space2) const {
  assert(space1 != nullptr);
  assert(space2 != nullptr);

  if (space1->subscriptIVs.empty() ||
      space1->subscriptIVs.size() != space2->subscriptIVs.size() ||
      space1->memoryMinusSCEV != space2->memoryMinusSCEV) {
    return false;
  }

  auto getLoopsForIV =
      [&](InductionVariable *iv) -> std::unordered_set<LoopStructure *> {
    auto stepPHIs = iv->getPHIsInvolvedInComputingIVStep();
    std::unordered_set<LoopStructure *> loopsForIV;
    for (auto *phi : stepPHIs) {
      loopsForIV.insert(this->loops->getInnermostLoopThatContains(phi));
    }
    return loopsForIV;
  };

  auto *rootLoopStructure = this->loops->getLoop();
  for (auto subscriptIdx = 0u; subscriptIdx < space1->subscriptIVs.size();
       ++subscriptIdx) {
    auto *iv1 = space1->subscriptIVs[subscriptIdx].second;
    auto *iv2 = space2->subscriptIVs[subscriptIdx].second;
    if (iv1 == nullptr || iv2 == nullptr) {
      return false;
    }

    auto loops1 = getLoopsForIV(iv1);
    auto loops2 = getLoopsForIV(iv2);
    if (loops1 != loops2) {
      for (auto *loop1 : loops1) {
        for (auto *loop2 : loops2) {
          if (rootLoopStructure == loop1 || rootLoopStructure == loop2) {
            return false;
          }
        }
      }
    }

    auto *scev1 = space1->subscripts[subscriptIdx];
    auto *scev2 = space2->subscripts[subscriptIdx];
    for (auto *loop : loops1) {
      if (rootLoopStructure == loop && scev1 != scev2) {
        return false;
      }
    }
  }

  return true;
}

void LoopIterationSpaceAnalysis::indexIVInstructionSCEVs(
    llvm::ScalarEvolution &SE) {
  for (auto *loop : this->loops->getLoops()) {
    for (auto *iv : this->ivManager.getInductionVariables(*loop)) {
      for (auto *inst : iv->getAllInstructions()) {
        if (!SE.isSCEVable(inst->getType())) {
          continue;
        }
        auto *scev = SE.getSCEV(inst);
        this->ivInstructionsBySCEV[scev].insert(inst);
        this->ivsByInstruction.insert(std::make_pair(inst, iv));
      }

      for (auto *inst : iv->getDerivedSCEVInstructions()) {
        if (!SE.isSCEVable(inst->getType())) {
          continue;
        }
        auto *scev = SE.getSCEV(inst);
        this->derivedInstructionsFromIVsBySCEV[scev].insert(inst);
        this->ivsByInstruction.insert(std::make_pair(inst, iv));
      }
    }
  }
}

void LoopIterationSpaceAnalysis::computeMemoryAccessSpace(
    llvm::ScalarEvolution &SE) {
  std::unordered_set<Instruction *> memoryAccessors{};
  auto *targetLoop = this->loops->getLoop();

  for (auto *B : targetLoop->getBasicBlocks()) {
    for (auto &I : *B) {
      Value *memoryAccessorValue = nullptr;
      if (auto *store = dyn_cast<StoreInst>(&I)) {
        memoryAccessorValue = store->getPointerOperand();
      } else if (auto *load = dyn_cast<LoadInst>(&I)) {
        memoryAccessorValue = load->getPointerOperand();
      } else if (auto *gep = dyn_cast<GetElementPtrInst>(&I)) {
        memoryAccessorValue = gep;
      } else {
        continue;
      }

      if (auto *memoryAccessor = dyn_cast<Instruction>(memoryAccessorValue)) {
        memoryAccessors.insert(memoryAccessor);
      }
    }
  }

  for (auto *memoryAccessor : memoryAccessors) {
    if (!SE.isSCEVable(memoryAccessor->getType())) {
      continue;
    }

    this->accessSpaces.push_back(
        std::make_unique<MemoryAccessSpace>(memoryAccessor));
    auto *memAccessSpace = this->accessSpaces.back().get();
    this->accessSpaceByInstruction[memoryAccessor] = memAccessSpace;

    memAccessSpace->memoryAccessorSCEV =
        SE.getSCEV(memAccessSpace->memoryAccessor);

    for (auto *user : memoryAccessor->users()) {
      if (isa<StoreInst>(user) || isa<LoadInst>(user) ||
          isa<GetElementPtrInst>(user)) {
        auto *accessor = cast<Instruction>(user);
        this->accessSpaceByInstruction[accessor] = memAccessSpace;
        memAccessSpace->accessInstructions.insert(accessor);
      }
    }

    Type *accessedType = nullptr;
    for (auto *accessor : memAccessSpace->accessInstructions) {
      if (auto *store = dyn_cast<StoreInst>(accessor)) {
        accessedType = store->getValueOperand()->getType();
      } else if (auto *load = dyn_cast<LoadInst>(accessor)) {
        accessedType = load->getType();
      } else if (auto *gep = dyn_cast<GetElementPtrInst>(accessor)) {
        accessedType = gep->getType();
      }
      if (accessedType != nullptr) {
        break;
      }
    }
    if (accessedType == nullptr) {
      continue;
    }

    auto *ptrToAccessedType = PointerType::getUnqual(accessedType);
    auto *efType = SE.getEffectiveSCEVType(ptrToAccessedType);
    memAccessSpace->elementSize = SE.getSizeOfExpr(efType, accessedType);
    if (memAccessSpace->elementSize == nullptr) {
      continue;
    }

    memAccessSpace->memoryAccessorBasePointerSCEV = dyn_cast<llvm::SCEVUnknown>(
        SE.getPointerBase(memAccessSpace->memoryAccessorSCEV));
    if (memAccessSpace->memoryAccessorBasePointerSCEV == nullptr) {
      continue;
    }

    auto *basePointer = memAccessSpace->memoryAccessorBasePointerSCEV;
    auto *accessFunction =
        SE.getMinusSCEV(memAccessSpace->memoryAccessorSCEV, basePointer);
    memAccessSpace->memoryMinusSCEV = accessFunction;
    SmallVector<const llvm::SCEV *, 4> delinearizedSubscripts;
    SmallVector<const llvm::SCEV *, 4> delinearizedSizes;
    llvm::delinearize(SE, accessFunction, delinearizedSubscripts,
                      delinearizedSizes, memAccessSpace->elementSize);
    memAccessSpace->subscripts.assign(delinearizedSubscripts.begin(),
                                      delinearizedSubscripts.end());
    memAccessSpace->sizes.assign(delinearizedSizes.begin(),
                                 delinearizedSizes.end());

    if (memAccessSpace->subscripts.empty()) {
      if (auto *gep =
              dyn_cast<GetElementPtrInst>(memAccessSpace->memoryAccessor)) {
        SmallVector<int, 4> sizes;
        SmallVector<const llvm::SCEV *, 4> gepSubscripts;
        llvm::getIndexExpressionsFromGEP(SE, gep, gepSubscripts, sizes);
        memAccessSpace->subscripts.assign(gepSubscripts.begin(),
                                          gepSubscripts.end());
        for (auto size : sizes) {
          memAccessSpace->sizes.push_back(
              SE.getConstant(accessFunction->getType(), size));
        }
        if (sizes.empty()) {
          memAccessSpace->sizes.push_back(memAccessSpace->elementSize);
        }
      }
    }

    bool isFullyDelinearized = true;
    for (auto *subscript : memAccessSpace->subscripts) {
      if (auto *addRecSubscript = dyn_cast<llvm::SCEVAddRecExpr>(subscript)) {
        if (isa<llvm::SCEVAddRecExpr>(addRecSubscript->getStart()) ||
            isa<llvm::SCEVAddRecExpr>(addRecSubscript->getStepRecurrence(SE))) {
          isFullyDelinearized = false;
          break;
        }
      }
    }
    if (isFullyDelinearized) {
      memAccessSpace->isAnalyzed = true;
    } else {
      memAccessSpace->subscripts.clear();
      memAccessSpace->sizes.clear();
    }
  }
}

void LoopIterationSpaceAnalysis::
    identifyNonOverlappingAccessesBetweenIterationsAcrossOneLoopInvocation(
        llvm::ScalarEvolution &SE) {
  for (auto &memAccessSpaceOwner : this->accessSpaces) {
    auto *memAccessSpace = memAccessSpaceOwner.get();
    if (memAccessSpace->subscriptIVs.empty() ||
        memAccessSpace->subscriptIVs.size() != memAccessSpace->sizes.size()) {
      continue;
    }

    bool isOverflowPossible = true;
    auto *scevExpression =
        dyn_cast<llvm::SCEVNAryExpr>(memAccessSpace->memoryAccessorSCEV);
    if (scevExpression != nullptr) {
      bool isBound = true;
      bool foundOffsetExpression = false;
      for (auto operandID = 0u; operandID < scevExpression->getNumOperands();
           operandID++) {
        auto *operand = scevExpression->getOperand(operandID);
        if (isa<llvm::SCEVConstant>(operand)) {
          continue;
        }
        if (auto *scevUnknown = dyn_cast<llvm::SCEVUnknown>(operand)) {
          auto *v = scevUnknown->getValue();
          if (isa<Argument>(v)) {
            continue;
          }
        }

        auto *offsetExpression = dyn_cast<llvm::SCEVNAryExpr>(operand);
        if (offsetExpression == nullptr) {
          isBound = false;
          break;
        }
        if (foundOffsetExpression) {
          isBound = false;
          break;
        }
        foundOffsetExpression = true;
        if (offsetExpression->hasNoSelfWrap() ||
            (offsetExpression->hasNoSignedWrap() &&
             offsetExpression->hasNoUnsignedWrap())) {
          continue;
        }
        isBound = false;
        break;
      }
      if (isBound && foundOffsetExpression) {
        isOverflowPossible = false;
      }
    }

    if (isOverflowPossible) {
      if (!isInnerDimensionSubscriptsBounded(SE, memAccessSpace)) {
        continue;
      }

      bool atLeastOneTopLevelNonOverlappingIV = false;
      auto *rootLoopStructure = this->loops->getLoop();
      for (auto idx = 0u; idx < memAccessSpace->subscriptIVs.size(); ++idx) {
        auto instIVPair = memAccessSpace->subscriptIVs[idx];
        auto *inst = instIVPair.first;
        auto *iv = instIVPair.second;
        if (iv == nullptr) {
          continue;
        }

        auto *loopEntryPHI = iv->getLoopEntryPHI();
        auto *loopStructure =
            this->loops->getInnermostLoopThatContains(loopEntryPHI);
        bool isRootLoopIV = (rootLoopStructure == loopStructure);
        if (!isRootLoopIV) {
          continue;
        }

        bool isIV = iv->isIVInstruction(inst);
        bool isDerivedFromIV = iv->isDerivedFromIVInstructions(inst);
        assert((isIV || isDerivedFromIV) &&
               "Subscript associated to IV has invalid associated instruction");

        bool isOneToOne = false;
        if (isIV) {
          bool isWrapping = false;
          isOneToOne = !isWrapping;
        } else {
          isOneToOne = isOneToOneFunctionOnIV(rootLoopStructure, iv, inst);
        }
        if (!isOneToOne) {
          continue;
        }
        atLeastOneTopLevelNonOverlappingIV |= isRootLoopIV;
      }

      if (!atLeastOneTopLevelNonOverlappingIV) {
        continue;
      }
    }

    this->nonOverlappingAccessesBetweenIterations.insert(memAccessSpace);
  }

  for (auto &accessSpacePair0 : this->accessSpaceByInstruction) {
    auto *accessSpace0 = accessSpacePair0.second;
    for (auto &accessSpacePair1 : this->accessSpaceByInstruction) {
      auto *accessSpace1 = accessSpacePair1.second;
      if (accessSpace0 == accessSpace1) {
        continue;
      }
      if (this->analyzeToCheckIfMemoryAccessSpaceNotOverlappingOrExactlyTheSame(
              accessSpace0, accessSpace1)) {
        this->spacesThatCannotOverlap[accessSpace0].insert(accessSpace1);
      }
    }
  }
}

void LoopIterationSpaceAnalysis::identifyIVForMemoryAccessSubscripts(
    llvm::ScalarEvolution &SE) {
  auto findCorrespondingIVForSubscript = [&](const llvm::SCEV *subscriptSCEV)
      -> std::pair<Instruction *, InductionVariable *> {
    auto emptyPair = std::make_pair(nullptr, nullptr);
    if (isa<llvm::SCEVConstant>(subscriptSCEV)) {
      return emptyPair;
    }

    auto scevsMatch = [](const llvm::SCEV *scev1,
                         const llvm::SCEV *scev2) -> bool {
      if (scev1 == scev2) {
        return true;
      }
      auto *scevConstant1 = dyn_cast<llvm::SCEVConstant>(scev1);
      auto *scevConstant2 = dyn_cast<llvm::SCEVConstant>(scev2);
      return scevConstant1 != nullptr && scevConstant2 != nullptr &&
             scevConstant1->getValue()->getSExtValue() ==
                 scevConstant2->getValue()->getSExtValue();
    };

    auto findInstructionInLoopForSCEV =
        [&SE, &scevsMatch](std::unordered_map<const llvm::SCEV *,
                                              std::unordered_set<Instruction *>>
                               &scevToInstMap,
                           const llvm::SCEV *subscriptSCEV) -> Instruction * {
      auto found = scevToInstMap.find(subscriptSCEV);
      if (found != scevToInstMap.end()) {
        return *found->second.begin();
      }

      if (auto *addRecSubscriptSCEV =
              dyn_cast<llvm::SCEVAddRecExpr>(subscriptSCEV)) {
        auto *loopHeader = addRecSubscriptSCEV->getLoop()->getHeader();
        for (auto &scevInstPair : scevToInstMap) {
          auto *otherAddRecSCEV =
              dyn_cast<llvm::SCEVAddRecExpr>(scevInstPair.first);
          if (otherAddRecSCEV == nullptr) {
            continue;
          }
          if (otherAddRecSCEV->getLoop()->getHeader() != loopHeader) {
            continue;
          }
          if (!scevsMatch(addRecSubscriptSCEV->getStart(),
                          otherAddRecSCEV->getStart()) ||
              !scevsMatch(addRecSubscriptSCEV->getStepRecurrence(SE),
                          otherAddRecSCEV->getStepRecurrence(SE))) {
            continue;
          }

          return *scevInstPair.second.begin();
        }
      }

      return nullptr;
    };

    auto *ivInst =
        findInstructionInLoopForSCEV(this->ivInstructionsBySCEV, subscriptSCEV);
    if (ivInst != nullptr) {
      return std::make_pair(ivInst, this->ivsByInstruction.at(ivInst));
    }

    auto *derivedInst = findInstructionInLoopForSCEV(
        this->derivedInstructionsFromIVsBySCEV, subscriptSCEV);
    if (derivedInst != nullptr) {
      return std::make_pair(derivedInst,
                            this->ivsByInstruction.at(derivedInst));
    }

    return emptyPair;
  };

  for (auto &memAccessSpaceOwner : this->accessSpaces) {
    auto *memAccessSpace = memAccessSpaceOwner.get();
    int idx = 0;
    for (auto *subscriptSCEV : memAccessSpace->subscripts) {
      if (isa<llvm::SCEVConstant>(subscriptSCEV)) {
        memAccessSpace->subscripts[idx] = memAccessSpace->memoryAccessorSCEV;
        subscriptSCEV = memAccessSpace->memoryAccessorSCEV;
      }
      memAccessSpace->subscriptIVs.push_back(
          findCorrespondingIVForSubscript(subscriptSCEV));
      idx++;
    }
    if (auto *phi = dyn_cast<PHINode>(memAccessSpace->memoryAccessor)) {
      if (idx == 0 && phi->getNumIncomingValues() == 1) {
        memAccessSpace->subscriptIVs.push_back(findCorrespondingIVForSubscript(
            memAccessSpace->memoryAccessorSCEV));
        assert(memAccessSpace->elementSize != nullptr &&
               "elementSize is nullptr");
        memAccessSpace->sizes.push_back(memAccessSpace->elementSize);
        auto *Expr = SE.getSCEV(phi->getIncomingValue(0));
        assert(Expr != nullptr && "Expr is nullptr");
        memAccessSpace->subscripts.push_back(Expr);
      }
    }
  }
}

LoopIterationSpaceAnalysis::MemoryAccessSpace::MemoryAccessSpace(
    Instruction *memoryAccessor)
    : memoryAccessor{memoryAccessor}, memoryAccessorSCEV{nullptr},
      memoryAccessorBasePointerSCEV{nullptr}, memoryMinusSCEV{nullptr},
      recurrence{nullptr}, elementSize{nullptr}, subscripts{}, sizes{},
      subscriptIVs{}, accessInstructions{}, constantStep{0}, isAnalyzed{false} {
}

bool LoopIterationSpaceAnalysis::isOneToOneFunctionOnIV(
    LoopStructure *loopStructure, InductionVariable *IV,
    Instruction *derivedInstruction) const {
  auto isInjectiveCast = [](Instruction *inst) -> bool {
    auto *castInst = dyn_cast<CastInst>(inst);
    if (castInst == nullptr) {
      return false;
    }

    auto *srcTy = castInst->getSrcTy();
    auto *dstTy = castInst->getDestTy();
    if (!srcTy->isIntegerTy() || !dstTy->isIntegerTy()) {
      return false;
    }

    auto srcWidth = cast<IntegerType>(srcTy)->getBitWidth();
    auto dstWidth = cast<IntegerType>(dstTy)->getBitWidth();
    if (isa<SExtInst>(castInst) || isa<ZExtInst>(castInst)) {
      return dstWidth >= srcWidth;
    }
    if (isa<BitCastInst>(castInst)) {
      return dstWidth == srcWidth;
    }

    return false;
  };

  std::queue<Instruction *> derivingInsts;
  std::unordered_set<Instruction *> visited;
  derivingInsts.push(derivedInstruction);

  while (!derivingInsts.empty()) {
    auto *inst = derivingInsts.front();
    derivingInsts.pop();
    if (IV->isIVInstruction(inst)) {
      continue;
    }

    auto op = inst->getOpcode();
    bool isOneToOne =
        (op == Instruction::Add || op == Instruction::Sub || isInjectiveCast(inst));
    if (!isOneToOne) {
      return false;
    }

    for (auto &use : inst->operands()) {
      auto *usedValue = use.get();
      if (isa<ConstantInt>(usedValue)) {
        continue;
      }
      auto *usedInst = dyn_cast<Instruction>(usedValue);
      if (usedInst == nullptr) {
        return false;
      }
      if (!loopStructure->isIncluded(usedInst)) {
        continue;
      }
      if (visited.find(usedInst) != visited.end()) {
        continue;
      }
      visited.insert(usedInst);
      derivingInsts.push(usedInst);
    }
  }

  return true;
}

bool LoopIterationSpaceAnalysis::isInnerDimensionSubscriptsBounded(
    llvm::ScalarEvolution &SE, MemoryAccessSpace *space) {
  if (space->subscriptIVs.empty() ||
      space->subscriptIVs.size() != space->sizes.size()) {
    return false;
  }

  auto scevsMatch = [](const llvm::SCEV *scev1,
                       const llvm::SCEV *scev2) -> bool {
    if (scev1 == scev2) {
      return true;
    }
    auto *constant1 = dyn_cast<llvm::SCEVConstant>(scev1);
    auto *constant2 = dyn_cast<llvm::SCEVConstant>(scev2);
    if (!constant1 || !constant2) {
      return false;
    }
    return constant1->getValue()->getSExtValue() ==
           constant2->getValue()->getSExtValue();
  };

  for (auto i = 1u; i < space->sizes.size(); ++i) {
    auto *sizeSCEV = space->sizes[i - 1];
    auto instIVPair = space->subscriptIVs[i];
    auto *inst = instIVPair.first;
    auto *iv = instIVPair.second;
    if (inst == nullptr) {
      return false;
    }

    auto *subscriptSCEV = SE.getSCEV(inst);
    if (subscriptSCEV == nullptr) {
      return false;
    }
    if (isa<llvm::SCEVSignExtendExpr>(subscriptSCEV) ||
        isa<llvm::SCEVTruncateExpr>(subscriptSCEV) ||
        isa<llvm::SCEVZeroExtendExpr>(subscriptSCEV)) {
      subscriptSCEV = cast<llvm::SCEVCastExpr>(subscriptSCEV)->getOperand();
    }
    if (!isa<IntegerType>(subscriptSCEV->getType())) {
      return false;
    }

    if (iv && iv->isIVInstruction(inst) &&
        isa<llvm::SCEVAddRecExpr>(subscriptSCEV)) {
      auto *subscriptRecSCEV = cast<llvm::SCEVAddRecExpr>(subscriptSCEV);
      auto *loopEntryPHI = iv->getLoopEntryPHI();
      auto *loopEntryPHISCEV =
          dyn_cast<llvm::SCEVAddRecExpr>(SE.getSCEV(loopEntryPHI));
      if (loopEntryPHISCEV == nullptr) {
        return false;
      }

      auto *stepSCEV = subscriptRecSCEV->getStepRecurrence(SE);
      auto *constantStepSCEV = dyn_cast<llvm::SCEVConstant>(stepSCEV);
      if (constantStepSCEV && constantStepSCEV->getValue()->isNegative()) {
        return false;
      }

      if (scevsMatch(subscriptRecSCEV->getStart(),
                     loopEntryPHISCEV->getStart()) &&
          scevsMatch(subscriptRecSCEV->getStepRecurrence(SE),
                     loopEntryPHISCEV->getStepRecurrence(SE))) {
        auto *loopHeader = loopEntryPHI->getParent();
        auto *loopStructure =
            this->loops->getInnermostLoopThatContains(loopHeader);
        if (loopStructure == nullptr) {
          return false;
        }
        auto *attr =
            ivManager.getLoopGoverningInductionVariable(*loopStructure);
        if (attr != nullptr && iv == attr->getInductionVariable()) {
          if (constantStepSCEV && !constantStepSCEV->getValue()->isNegative()) {
            auto *conditionValue = attr->getExitConditionValue();
            auto *cmpInst =
                attr->getHeaderCompareInstructionToComputeExitCondition();
            if (conditionValue == nullptr || cmpInst == nullptr) {
              return false;
            }

            auto predicate = cmpInst->getPredicate();
            auto *exitBlock = attr->getExitBlockFromHeader();
            auto *falseSuccessor = *(++succ_begin(loopHeader));
            bool exitOnFalse = exitBlock == falseSuccessor;
            bool isConditionLHS = cmpInst->getOperand(0) == conditionValue;

            if (predicate == ICmpInst::Predicate::ICMP_ULE ||
                predicate == ICmpInst::Predicate::ICMP_SLE) {
              predicate = ICmpInst::Predicate::ICMP_UGT;
              isConditionLHS = !isConditionLHS;
            } else if (predicate == ICmpInst::Predicate::ICMP_UGE ||
                       predicate == ICmpInst::Predicate::ICMP_SGE) {
              predicate = ICmpInst::Predicate::ICMP_ULT;
              isConditionLHS = !isConditionLHS;
            }

            bool isUpperBoundedByEq =
                (!exitOnFalse && predicate == ICmpInst::Predicate::ICMP_EQ) ||
                (exitOnFalse && predicate == ICmpInst::Predicate::ICMP_NE);
            bool isUpperBoundedByLT =
                exitOnFalse && !isConditionLHS &&
                (predicate == ICmpInst::Predicate::ICMP_ULT ||
                 predicate == ICmpInst::Predicate::ICMP_SLT);
            bool isUpperBoundedByFlippedGT =
                exitOnFalse && isConditionLHS &&
                (predicate == ICmpInst::Predicate::ICMP_UGT ||
                 predicate == ICmpInst::Predicate::ICMP_SGT);
            bool isUpperBounded = isUpperBoundedByEq || isUpperBoundedByLT ||
                                  isUpperBoundedByFlippedGT;

            if (isUpperBounded) {
              auto *conditionSCEVBase = SE.getSCEV(conditionValue);
              auto *sizeSCEVBase = sizeSCEV;
              auto *operand = conditionSCEVBase;
              if (isa<llvm::SCEVSignExtendExpr>(operand) ||
                  isa<llvm::SCEVTruncateExpr>(operand) ||
                  isa<llvm::SCEVZeroExtendExpr>(operand)) {
                conditionSCEVBase =
                    cast<llvm::SCEVCastExpr>(operand)->getOperand();
              }
              operand = sizeSCEVBase;
              if (isa<llvm::SCEVSignExtendExpr>(operand) ||
                  isa<llvm::SCEVTruncateExpr>(operand) ||
                  isa<llvm::SCEVZeroExtendExpr>(operand)) {
                sizeSCEVBase = cast<llvm::SCEVCastExpr>(operand)->getOperand();
              }

              if (conditionSCEVBase == sizeSCEVBase) {
                continue;
              }

              if (auto *conditionOffsetSCEV =
                      dyn_cast<llvm::SCEVAddExpr>(conditionSCEVBase)) {
                if (conditionOffsetSCEV->getNumOperands() == 2) {
                  auto *lhsSCEV = conditionOffsetSCEV->getOperand(0);
                  auto *rhsSCEV = conditionOffsetSCEV->getOperand(1);
                  if ((lhsSCEV == sizeSCEVBase) ^ (rhsSCEV == sizeSCEVBase)) {
                    auto *otherSCEV =
                        lhsSCEV == sizeSCEVBase ? rhsSCEV : lhsSCEV;
                    if (auto *constOffsetSCEV =
                            dyn_cast<llvm::SCEVConstant>(otherSCEV)) {
                      if (constOffsetSCEV->getValue()->isNegative()) {
                        continue;
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

    return false;
  }

  return true;
}

} // namespace loop
} // namespace analysis
} // namespace lotus
