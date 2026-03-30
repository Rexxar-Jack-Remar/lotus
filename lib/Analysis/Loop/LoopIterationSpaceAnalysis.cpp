/*
 * Copyright 2026 Lotus contributors
 */
#include "Analysis/Loop/LoopIterationSpaceAnalysis.h"

namespace lotus {
namespace analysis {
namespace loop {

LoopIterationSpaceAnalysis::MemoryAccessSpace::MemoryAccessSpace(
    Instruction *memoryAccessor)
    : memoryAccessor{memoryAccessor},
      memoryAccessorSCEV{nullptr},
      memoryAccessorBasePointerSCEV{nullptr},
      memoryMinusSCEV{nullptr},
      recurrence{nullptr},
      elementSize{nullptr},
      subscripts{},
      sizes{},
      subscriptIVs{},
      accessInstructions{},
      constantStep{0},
      isAnalyzed{false} {}

LoopIterationSpaceAnalysis::LoopIterationSpaceAnalysis(
    LoopTree *loops,
    InductionVariableManager &ivManager,
    llvm::ScalarEvolution &SE)
    : loops{loops}, ivManager{ivManager} {
  indexIVInstructionSCEVs(SE);
  computeMemoryAccessSpace(SE);
  identifyIVForMemoryAccessSubscripts(SE);
}

void LoopIterationSpaceAnalysis::indexIVInstructionSCEVs(llvm::ScalarEvolution &SE) {
  for (auto *loop : this->loops->getLoops()) {
    for (auto *iv : this->ivManager.getInductionVariables(*loop)) {
      for (auto *inst : iv->getAllInstructions()) {
        if (!SE.isSCEVable(inst->getType())) {
          continue;
        }
        auto *scev = SE.getSCEV(inst);
        this->ivInstructionsBySCEV[scev].insert(inst);
        this->ivsByInstruction[inst] = iv;
      }
      for (auto *inst : iv->getDerivedSCEVInstructions()) {
        if (!SE.isSCEVable(inst->getType())) {
          continue;
        }
        auto *scev = SE.getSCEV(inst);
        this->derivedInstructionsFromIVsBySCEV[scev].insert(inst);
        this->ivsByInstruction[inst] = iv;
      }
    }
  }
}

void LoopIterationSpaceAnalysis::computeMemoryAccessSpace(llvm::ScalarEvolution &SE) {
  auto *targetLoop = this->loops->getLoop();
  for (auto *bb : targetLoop->getBasicBlocks()) {
    for (auto &inst : *bb) {
      Value *ptr = nullptr;
      Type *accessedType = nullptr;
      if (auto *load = dyn_cast<LoadInst>(&inst)) {
        ptr = load->getPointerOperand();
        accessedType = load->getType();
      } else if (auto *store = dyn_cast<StoreInst>(&inst)) {
        ptr = store->getPointerOperand();
        accessedType = store->getValueOperand()->getType();
      } else {
        continue;
      }

      auto *ptrInst = dyn_cast<Instruction>(ptr);
      if (ptrInst == nullptr || !SE.isSCEVable(ptrInst->getType())) {
        continue;
      }

      auto info = std::make_unique<MemoryAccessSpace>(&inst);
      info->accessInstructions.insert(&inst);
      info->memoryAccessorSCEV = SE.getSCEV(ptrInst);
      info->memoryAccessorBasePointerSCEV =
          dyn_cast<llvm::SCEVUnknown>(SE.getPointerBase(info->memoryAccessorSCEV));
      if (info->memoryAccessorBasePointerSCEV == nullptr) {
        continue;
      }

      info->memoryMinusSCEV = SE.getMinusSCEV(info->memoryAccessorSCEV,
                                              info->memoryAccessorBasePointerSCEV);
      info->recurrence = dyn_cast<llvm::SCEVAddRecExpr>(info->memoryMinusSCEV);

      if (accessedType != nullptr) {
        auto *ptrToAccessedType = PointerType::getUnqual(accessedType);
        auto *effectiveType = SE.getEffectiveSCEVType(ptrToAccessedType);
        info->elementSize = SE.getSizeOfExpr(effectiveType, accessedType);
      }

      if (auto *gep = dyn_cast<GetElementPtrInst>(ptrInst)) {
        for (auto idx = gep->idx_begin(); idx != gep->idx_end(); ++idx) {
          auto *indexOperand = idx->get();
          if (!SE.isSCEVable(indexOperand->getType())) {
            continue;
          }
          info->subscripts.push_back(SE.getSCEV(indexOperand));
        }
      } else if (info->recurrence != nullptr) {
        info->subscripts.push_back(info->recurrence);
      }

      if (info->recurrence != nullptr) {
        if (auto *step =
                dyn_cast<llvm::SCEVConstant>(info->recurrence->getStepRecurrence(SE))) {
          info->constantStep = step->getValue()->getSExtValue();
          info->isAnalyzed = info->constantStep != 0;
        }
      }

      if (!info->subscripts.empty()) {
        info->isAnalyzed = true;
      }
      this->accessSpaceByInstruction[&inst] = std::move(info);
    }
  }
}

void LoopIterationSpaceAnalysis::identifyIVForMemoryAccessSubscripts(
    llvm::ScalarEvolution &SE) {
  auto scevsMatch = [](const llvm::SCEV *lhs, const llvm::SCEV *rhs) {
    if (lhs == rhs) {
      return true;
    }
    auto *lc = dyn_cast<llvm::SCEVConstant>(lhs);
    auto *rc = dyn_cast<llvm::SCEVConstant>(rhs);
    return lc != nullptr && rc != nullptr
           && lc->getValue()->getSExtValue() == rc->getValue()->getSExtValue();
  };

  auto findInstructionInLoopForSCEV =
      [&scevsMatch, &SE](
          const std::unordered_map<const llvm::SCEV *, std::unordered_set<Instruction *>> &map,
          const llvm::SCEV *subscriptSCEV) -> Instruction * {
    auto exact = map.find(subscriptSCEV);
    if (exact != map.end() && !exact->second.empty()) {
      return *exact->second.begin();
    }
    auto *addRec = dyn_cast<llvm::SCEVAddRecExpr>(subscriptSCEV);
    if (addRec == nullptr) {
      return nullptr;
    }
    for (auto const &pair : map) {
      auto *otherAddRec = dyn_cast<llvm::SCEVAddRecExpr>(pair.first);
      if (otherAddRec == nullptr) {
        continue;
      }
      if (otherAddRec->getLoop()->getHeader() != addRec->getLoop()->getHeader()) {
        continue;
      }
      if (!scevsMatch(otherAddRec->getStart(), addRec->getStart())) {
        continue;
      }
      if (!scevsMatch(otherAddRec->getStepRecurrence(SE), addRec->getStepRecurrence(SE))) {
        continue;
      }
      return *pair.second.begin();
    }
    return nullptr;
  };

  for (auto &pair : this->accessSpaceByInstruction) {
    auto *space = pair.second.get();
    space->subscriptIVs.clear();
    for (auto *subscriptSCEV : space->subscripts) {
      if (isa<llvm::SCEVConstant>(subscriptSCEV)) {
        space->subscriptIVs.emplace_back(nullptr, nullptr);
        continue;
      }

      auto *ivInst = findInstructionInLoopForSCEV(this->ivInstructionsBySCEV, subscriptSCEV);
      if (ivInst != nullptr) {
        space->subscriptIVs.emplace_back(ivInst, this->ivsByInstruction.at(ivInst));
        continue;
      }

      auto *derivedInst =
          findInstructionInLoopForSCEV(this->derivedInstructionsFromIVsBySCEV, subscriptSCEV);
      if (derivedInst != nullptr) {
        space->subscriptIVs.emplace_back(derivedInst, this->ivsByInstruction.at(derivedInst));
        continue;
      }

      space->subscriptIVs.emplace_back(nullptr, nullptr);
    }
  }
}

bool LoopIterationSpaceAnalysis::isMemoryAccessSpaceEquivalentForTopLoopIVSubscript(
    MemoryAccessSpace *space1,
    MemoryAccessSpace *space2) const {
  if (space1 == nullptr || space2 == nullptr) {
    return false;
  }
  if (space1->memoryAccessorBasePointerSCEV != space2->memoryAccessorBasePointerSCEV) {
    return false;
  }
  if (space1->subscripts.size() != space2->subscripts.size()) {
    return false;
  }
  auto *rootLoop = this->loops->getLoop();
  bool foundTopLevelEquivalent = false;
  for (size_t i = 0; i < space1->subscriptIVs.size(); ++i) {
    auto inst1 = space1->subscriptIVs[i].first;
    auto iv1 = space1->subscriptIVs[i].second;
    auto inst2 = space2->subscriptIVs[i].first;
    auto iv2 = space2->subscriptIVs[i].second;
    if (iv1 == nullptr || iv2 == nullptr) {
      continue;
    }
    auto *loop1 = this->loops->getInnermostLoopThatContains(iv1->getLoopEntryPHI());
    auto *loop2 = this->loops->getInnermostLoopThatContains(iv2->getLoopEntryPHI());
    if (loop1 == rootLoop || loop2 == rootLoop) {
      if (inst1 == inst2 || space1->subscripts[i] == space2->subscripts[i]) {
        foundTopLevelEquivalent = true;
        continue;
      }
      return false;
    }
  }
  return foundTopLevelEquivalent;
}

bool LoopIterationSpaceAnalysis::isOneToOneFunctionOnIV(
    LoopStructure *loopStructure,
    InductionVariable *IV,
    Instruction *derivedInstruction) const {
  std::queue<Instruction *> derivingInsts;
  std::unordered_set<Instruction *> visited;
  derivingInsts.push(derivedInstruction);
  visited.insert(derivedInstruction);

  while (!derivingInsts.empty()) {
    auto *inst = derivingInsts.front();
    derivingInsts.pop();
    if (IV->isIVInstruction(inst)) {
      continue;
    }

    auto op = inst->getOpcode();
    bool isOneToOne = (op == Instruction::Add || op == Instruction::Sub
                       || op == Instruction::Mul || inst->isCast());
    if (!isOneToOne) {
      return false;
    }

    for (auto &use : inst->operands()) {
      auto *usedValue = use.get();
      if (isa<ConstantInt>(usedValue)) {
        continue;
      }
      auto *usedInst = dyn_cast<Instruction>(usedValue);
      if (usedInst == nullptr || !loopStructure->isIncluded(usedInst)) {
        continue;
      }
      if (visited.insert(usedInst).second) {
        derivingInsts.push(usedInst);
      }
    }
  }
  return true;
}

bool LoopIterationSpaceAnalysis::
    areInstructionsAccessingDisjointMemoryLocationsBetweenIterations(
        Instruction *from,
        Instruction *to) const {
  auto fromIt = this->accessSpaceByInstruction.find(from);
  auto toIt = this->accessSpaceByInstruction.find(to);
  if (fromIt == this->accessSpaceByInstruction.end()
      || toIt == this->accessSpaceByInstruction.end()) {
    return false;
  }

  auto *fromInfo = fromIt->second.get();
  auto *toInfo = toIt->second.get();
  if (!fromInfo->isAnalyzed || !toInfo->isAnalyzed) {
    return false;
  }

  if (fromInfo->memoryAccessorSCEV == toInfo->memoryAccessorSCEV
      || fromInfo->memoryMinusSCEV == toInfo->memoryMinusSCEV) {
    return true;
  }

  if (fromInfo->memoryAccessorBasePointerSCEV != toInfo->memoryAccessorBasePointerSCEV) {
    return false;
  }

  if (fromInfo == toInfo) {
    return true;
  }

  if (fromInfo->recurrence != nullptr && toInfo->recurrence != nullptr) {
    if (fromInfo->recurrence == toInfo->recurrence
        && fromInfo->constantStep == toInfo->constantStep) {
      return true;
    }
  }

  if (isMemoryAccessSpaceEquivalentForTopLoopIVSubscript(fromInfo, toInfo)) {
    return true;
  }

  for (auto const &subscriptPair : fromInfo->subscriptIVs) {
    auto *inst = subscriptPair.first;
    auto *iv = subscriptPair.second;
    if (inst == nullptr || iv == nullptr) {
      continue;
    }
    auto *ivLoop = this->loops->getInnermostLoopThatContains(iv->getLoopEntryPHI());
    if (ivLoop != this->loops->getLoop()) {
      continue;
    }
    if (inst == from || inst == to) {
      return true;
    }
    if (iv->isDerivedFromIVInstructions(inst)
        && isOneToOneFunctionOnIV(this->loops->getLoop(), iv, inst)) {
      return true;
    }
  }

  return false;
}

} // namespace loop
} // namespace analysis
} // namespace lotus
