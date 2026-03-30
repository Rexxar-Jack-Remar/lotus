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
      recurrence{nullptr},
      constantStep{0},
      isAnalyzed{false} {}

LoopIterationSpaceAnalysis::LoopIterationSpaceAnalysis(
    LoopTree *loops,
    InductionVariableManager &ivManager,
    llvm::ScalarEvolution &SE)
    : loops{loops}, ivManager{ivManager} {
  computeMemoryAccessSpace(SE);
}

void LoopIterationSpaceAnalysis::computeMemoryAccessSpace(llvm::ScalarEvolution &SE) {
  auto *targetLoop = this->loops->getLoop();
  for (auto *bb : targetLoop->getBasicBlocks()) {
    for (auto &inst : *bb) {
      Value *ptr = nullptr;
      if (auto *load = dyn_cast<LoadInst>(&inst)) {
        ptr = load->getPointerOperand();
      } else if (auto *store = dyn_cast<StoreInst>(&inst)) {
        ptr = store->getPointerOperand();
      } else {
        continue;
      }

      auto *ptrInst = dyn_cast<Instruction>(ptr);
      if (ptrInst == nullptr || !SE.isSCEVable(ptrInst->getType())) {
        continue;
      }

      auto info = std::make_unique<MemoryAccessSpace>(&inst);
      info->memoryAccessorSCEV = SE.getSCEV(ptrInst);
      info->memoryAccessorBasePointerSCEV =
          dyn_cast<llvm::SCEVUnknown>(SE.getPointerBase(info->memoryAccessorSCEV));
      if (info->memoryAccessorBasePointerSCEV == nullptr) {
        continue;
      }

      auto *delta = SE.getMinusSCEV(info->memoryAccessorSCEV,
                                    info->memoryAccessorBasePointerSCEV);
      info->recurrence = dyn_cast<llvm::SCEVAddRecExpr>(delta);
      if (info->recurrence == nullptr) {
        continue;
      }
      auto *step =
          dyn_cast<llvm::SCEVConstant>(info->recurrence->getStepRecurrence(SE));
      if (step == nullptr) {
        continue;
      }
      info->constantStep = step->getValue()->getSExtValue();
      if (info->constantStep == 0) {
        continue;
      }
      info->isAnalyzed = true;
      this->accessSpaceByInstruction[&inst] = std::move(info);
    }
  }
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
  if (fromInfo->memoryAccessorBasePointerSCEV != toInfo->memoryAccessorBasePointerSCEV) {
    return false;
  }
  return fromInfo->recurrence == toInfo->recurrence
         && fromInfo->constantStep == toInfo->constantStep;
}

} // namespace loop
} // namespace analysis
} // namespace lotus
