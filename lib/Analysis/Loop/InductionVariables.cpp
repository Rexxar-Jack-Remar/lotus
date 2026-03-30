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

void collectIVInstructions(LoopStructure *loop,
                           LoopSCC *scc,
                           PHINode *phi,
                           std::unordered_set<PHINode *> &phis,
                           std::unordered_set<Instruction *> &nonPHIInstructions,
                           std::unordered_set<Instruction *> &instructions) {
  if (scc != nullptr) {
    for (auto &pair : scc->internalNodePairs()) {
      auto *inst = dyn_cast_or_null<Instruction>(pair.first);
      if (inst == nullptr || !loop->isIncluded(inst)) {
        continue;
      }
      instructions.insert(inst);
      if (auto *innerPhi = dyn_cast<PHINode>(inst)) {
        phis.insert(innerPhi);
      } else {
        nonPHIInstructions.insert(inst);
      }
    }
  }

  if (instructions.empty()) {
    instructions.insert(phi);
    phis.insert(phi);
    for (unsigned i = 0; i < phi->getNumIncomingValues(); ++i) {
      auto *incomingBlock = phi->getIncomingBlock(i);
      if (!loop->isIncluded(incomingBlock)) {
        continue;
      }
      if (auto *inst = dyn_cast<Instruction>(phi->getIncomingValue(i))) {
        instructions.insert(inst);
        if (auto *innerPhi = dyn_cast<PHINode>(inst)) {
          phis.insert(innerPhi);
        } else {
          nonPHIInstructions.insert(inst);
        }
      }
    }
  }

  std::vector<Instruction *> worklist(instructions.begin(), instructions.end());
  while (!worklist.empty()) {
    auto *current = worklist.back();
    worklist.pop_back();
    for (auto *user : current->users()) {
      auto *userInst = dyn_cast<Instruction>(user);
      if (userInst == nullptr || !loop->isIncluded(userInst) || !isa<CastInst>(userInst)) {
        continue;
      }
      if (instructions.insert(userInst).second) {
        nonPHIInstructions.insert(userInst);
        worklist.push_back(userInst);
      }
    }
  }
}

bool isSCEVDerivedInstruction(LoopStructure *loop,
                              InvariantManager &IVM,
                              llvm::ScalarEvolution &SE,
                              const std::unordered_set<Instruction *> &ivInstructions,
                              const std::unordered_set<Instruction *> &alreadyDerived,
                              Instruction *inst) {
  if (inst == nullptr || !loop->isIncluded(inst) || !SE.isSCEVable(inst->getType())) {
    return false;
  }

  auto *scev = SE.getSCEV(inst);
  bool supportedSCEV = isa<llvm::SCEVCastExpr>(scev)
                       || isa<llvm::SCEVNAryExpr>(scev)
                       || isa<llvm::SCEVUDivExpr>(scev);
  if (!supportedSCEV && !inst->isBinaryOp()) {
    return false;
  }

  bool usesAtLeastOneIVInstruction = false;
  for (auto &operandUse : inst->operands()) {
    auto *operand = operandUse.get();
    if (isa<ConstantInt>(operand) || IVM.isLoopInvariant(operand)) {
      continue;
    }
    auto *operandInst = dyn_cast<Instruction>(operand);
    if (operandInst == nullptr) {
      return false;
    }
    if (!loop->isIncluded(operandInst)) {
      continue;
    }
    if (ivInstructions.count(operandInst) != 0 || alreadyDerived.count(operandInst) != 0) {
      usesAtLeastOneIVInstruction = true;
      continue;
    }
    return false;
  }

  return usesAtLeastOneIVInstruction;
}

std::unordered_set<Instruction *> collectDerivedSCEVInstructions(
    LoopStructure *loop,
    InvariantManager &IVM,
    llvm::ScalarEvolution &SE,
    const std::unordered_set<Instruction *> &ivInstructions) {
  std::unordered_set<Instruction *> derivedInstructions;
  std::queue<Instruction *> worklist;
  std::unordered_set<Instruction *> visited;
  for (auto *inst : ivInstructions) {
    worklist.push(inst);
    visited.insert(inst);
  }

  while (!worklist.empty()) {
    auto *inst = worklist.front();
    worklist.pop();
    for (auto *user : inst->users()) {
      auto *userInst = dyn_cast<Instruction>(user);
      if (userInst == nullptr || visited.count(userInst) != 0) {
        continue;
      }
      visited.insert(userInst);
      if (!isSCEVDerivedInstruction(
              loop, IVM, SE, ivInstructions, derivedInstructions, userInst)) {
        continue;
      }
      derivedInstructions.insert(userInst);
      worklist.push(userInst);
    }
  }
  return derivedInstructions;
}

} // namespace

InductionVariableManager::InductionVariableManager(
    LoopTree *loop,
    InvariantManager &IVM,
    llvm::ScalarEvolution &SE,
    llvm::LoopInfo &LI,
    LoopSCCDAG &sccdag)
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

      auto *ivSCC = sccdag.getSCC(&phi);
      std::unordered_set<PHINode *> stepPHIs{&phi};
      std::unordered_set<PHINode *> phis;
      std::unordered_set<Instruction *> nonPHIInstructions;
      std::unordered_set<Instruction *> instructions;
      collectIVInstructions(
          loopStructure, ivSCC, &phi, phis, nonPHIInstructions, instructions);
      auto derivedInstructions =
          collectDerivedSCEVInstructions(loopStructure, IVM, SE, instructions);

      owned.emplace_back(new InductionVariable(loopStructure,
                                               ivSCC,
                                               &phi,
                                               startValue,
                                               stepSCEV,
                                               singleStepValue,
                                               stepPHIs,
                                               phis,
                                               nonPHIInstructions,
                                               instructions,
                                               derivedInstructions));
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
