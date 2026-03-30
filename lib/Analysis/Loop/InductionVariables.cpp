/*
 * Copyright 2026 Lotus contributors
 */
#include "Analysis/Loop/InductionVariables.h"

namespace lotus {
namespace analysis {
namespace loop {

namespace {

Value *extractSingleStepValue(const llvm::SCEV *stepSCEV, LoopStructure *loop) {
  if (stepSCEV == nullptr) {
    return nullptr;
  }
  if (auto *constant = dyn_cast<llvm::SCEVConstant>(stepSCEV)) {
    return constant->getValue();
  }
  if (auto *unknown = dyn_cast<llvm::SCEVUnknown>(stepSCEV)) {
    auto *value = unknown->getValue();
    if (loop != nullptr && loop->isLoopInvariant(value)) {
      return value;
    }
  }
  return nullptr;
}

std::unordered_set<Instruction *> collectIVInstructions(LoopStructure *loop,
                                                        PHINode *phi) {
  std::unordered_set<Instruction *> instructions;
  instructions.insert(phi);
  for (unsigned i = 0; i < phi->getNumIncomingValues(); ++i) {
    auto *incomingBlock = phi->getIncomingBlock(i);
    if (loop->isIncluded(incomingBlock)) {
      if (auto *inst = dyn_cast<Instruction>(phi->getIncomingValue(i))) {
        instructions.insert(inst);
      }
    }
  }
  return instructions;
}

} // namespace

InductionVariableManager::InductionVariableManager(
    LoopTree *loop,
    InvariantManager &,
    llvm::ScalarEvolution &SE,
    llvm::LoopInfo &LI,
    LoopSCCDAG &)
    : loop{loop} {
  assert(loop != nullptr);
  auto loops = loop->getLoops();
  for (auto *loopStructure : loops) {
    auto *llvmLoop = LI.getLoopFor(loopStructure->getHeader());
    if (llvmLoop == nullptr) {
      continue;
    }
    auto *preheader = loopStructure->getPreHeader();
    if (preheader == nullptr) {
      continue;
    }

    auto &owned = this->ownedIVs[loopStructure];
    for (auto &phi : loopStructure->getHeader()->phis()) {
      if (phi.getBasicBlockIndex(preheader) < 0) {
        continue;
      }

      llvm::InductionDescriptor descriptor;
      bool isValid =
          llvm::InductionDescriptor::isInductionPHI(&phi, llvmLoop, &SE, descriptor);
      if (!isValid && phi.getType()->isFloatingPointTy()) {
        isValid = llvm::InductionDescriptor::isFPInductionPHI(
            &phi, llvmLoop, &SE, descriptor);
      }
      if (!isValid) {
        continue;
      }

      Value *startValue = descriptor.getStartValue();
      auto *stepSCEV = descriptor.getStep();
      Value *singleStepValue = descriptor.getConstIntStepValue();
      if (singleStepValue == nullptr) {
        singleStepValue = extractSingleStepValue(stepSCEV, loopStructure);
      }
      if (singleStepValue == nullptr) {
        continue;
      }

      owned.emplace_back(new InductionVariable(loopStructure,
                                               &phi,
                                               startValue,
                                               stepSCEV,
                                               singleStepValue,
                                               collectIVInstructions(
                                                   loopStructure, &phi)));
    }

    for (auto &iv : owned) {
      auto candidate =
          std::unique_ptr<LoopGoverningInductionVariable>(
              new LoopGoverningInductionVariable(loopStructure, *iv));
      if (!candidate->isSCCContainingIVWellFormed()) {
        continue;
      }
      this->governingIVs[loopStructure] = candidate.get();
      this->ownedGoverningIVs.push_back(std::move(candidate));
      break;
    }
  }
}

std::unordered_set<InductionVariable *>
InductionVariableManager::getInductionVariables(void) const {
  auto *root = this->loop->getLoop();
  return this->getInductionVariables(*root);
}

std::unordered_set<InductionVariable *>
InductionVariableManager::getInductionVariables(LoopStructure &loop) const {
  std::unordered_set<InductionVariable *> ivs;
  auto it = this->ownedIVs.find(&loop);
  if (it == this->ownedIVs.end()) {
    return ivs;
  }
  for (auto const &owned : it->second) {
    ivs.insert(owned.get());
  }
  return ivs;
}

LoopGoverningInductionVariable *
InductionVariableManager::getLoopGoverningInductionVariable(void) const {
  auto *root = this->loop->getLoop();
  return this->getLoopGoverningInductionVariable(*root);
}

LoopGoverningInductionVariable *
InductionVariableManager::getLoopGoverningInductionVariable(
    LoopStructure &loop) const {
  auto it = this->governingIVs.find(&loop);
  if (it == this->governingIVs.end()) {
    return nullptr;
  }
  return it->second;
}

} // namespace loop
} // namespace analysis
} // namespace lotus
