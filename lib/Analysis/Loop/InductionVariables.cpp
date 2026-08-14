/*
 * Copyright 2026 Lotus contributors
 */
#include "Analysis/Loop/InductionVariables.h"

namespace lotus {
namespace analysis {
namespace loop {

namespace {} // namespace

InductionVariableManager::InductionVariableManager(
    LoopTree *loopNode, InvariantManager &IVM, llvm::ScalarEvolution &SE,
    llvm::LoopInfo &LI, LoopSCCDAG &sccdag, LoopEnvironment &loopEnvironment,
    bool enableExtendedRecognition)
    : loop{loopNode}, ownedIVs{}, governingIVs{}, ownedGoverningIVs{} {
  assert(this->loop != nullptr);

  auto *loopToAnalyze = this->loop->getLoop();
  assert(loopToAnalyze != nullptr);
  auto &F = *loopToAnalyze->getHeader()->getParent();
  llvm::ScalarEvolutionReferentialExpander referentialExpander(SE, F);

  for (auto *loop : this->loop->getLoops()) {
    auto &owned = this->ownedIVs[loop];
    auto *header = loop->getHeader();
    auto *preHeader = loop->getPreHeader();
    if (header == nullptr) {
      continue;
    }

    auto *llvmLoop = LI.getLoopFor(header);
    for (auto &phi : header->phis()) {
      llvm::InductionDescriptor ID{};
      bool llvmDeterminedValidIV = false;
      bool llvmLoopValidForInductionAnalysis =
          preHeader != nullptr && (phi.getBasicBlockIndex(preHeader) >= 0) &&
          (llvmLoop != nullptr);
      if (llvmLoopValidForInductionAnalysis &&
          llvm::InductionDescriptor::isInductionPHI(&phi, llvmLoop, &SE, ID)) {
        llvmDeterminedValidIV = true;
      } else if (phi.getType()->isFloatingPointTy() &&
                 llvmLoop != nullptr &&
                 llvm::InductionDescriptor::isFPInductionPHI(&phi, llvmLoop,
                                                             &SE, ID)) {
        llvmDeterminedValidIV = true;
      }

      auto *sccContainingIV = sccdag.getSCC(&phi);
      bool noelleDeterminedValidIV = true;
      std::unique_ptr<InductionVariable> IV;
      if (!SE.isSCEVable(phi.getType())) {
        noelleDeterminedValidIV = false;
      } else {
        auto *scev = SE.getSCEV(&phi);
        if (scev == nullptr) {
          noelleDeterminedValidIV = false;
        } else if (scev->getSCEVType() != llvm::SCEVTypes::scAddRecExpr) {
          noelleDeterminedValidIV = false;
          int64_t stepMultiplier = 1;

          bool foundOnePHI = false;
          PHINode *internalPHI = nullptr;
          if (sccContainingIV != nullptr) {
            sccContainingIV->iterateOverInstructions(
                [&](Instruction *I) -> bool {
                  if (isa<PHINode>(I) && I != &phi &&
                      SE.getSCEV(I)->getSCEVType() ==
                          llvm::SCEVTypes::scAddRecExpr &&
                      this->loop->isIncludedInItsSubLoops(I)) {
                    if (!foundOnePHI) {
                      foundOnePHI = true;
                      internalPHI = cast<PHINode>(I);
                    } else {
                      foundOnePHI = false;
                      return true;
                    }
                  }
                  return false;
                });
          }

          if (!foundOnePHI) {
            goto allocate_iv;
          }

          auto *subloop = this->loop->getInnermostLoopThatContains(internalPHI);
          if (subloop == nullptr ||
              subloop->getLoopExitBasicBlocks().size() != 1) {
            goto allocate_iv;
          }

          auto *subloopHeader = subloop->getHeader();
          if (subloopHeader->getUniqueSuccessor() != nullptr) {
            goto allocate_iv;
          }

          auto *llvmSubloop = LI.getLoopFor(subloopHeader);
          if (llvmSubloop == nullptr) {
            goto allocate_iv;
          }
          auto exactTripCount = SE.getSmallConstantTripCount(llvmSubloop);
          if (exactTripCount != 0) {
            stepMultiplier = exactTripCount;
            IV.reset(new InductionVariable(
                loop, IVM, SE, stepMultiplier, &phi,
                std::unordered_set<PHINode *>({internalPHI}), sccContainingIV,
                loopEnvironment, referentialExpander));
          }
        }
      }

    allocate_iv:
      if (enableExtendedRecognition &&
          !IV && (noelleDeterminedValidIV || llvmDeterminedValidIV)) {
        Value *startValue =
            llvmDeterminedValidIV ? ID.getStartValue() : nullptr;
        if (!llvmDeterminedValidIV) {
          auto bbs = loop->getBasicBlocks();
          for (auto i = 0u; i < phi.getNumIncomingValues(); ++i) {
            auto *incomingBB = phi.getIncomingBlock(i);
            if (bbs.find(incomingBB) == bbs.end()) {
              startValue = phi.getIncomingValue(i);
              break;
            }
          }
        }
        auto *stepSCEV = llvmDeterminedValidIV ? ID.getStep() : nullptr;
        Value *singleStepValue =
            llvmDeterminedValidIV ? ID.getConstIntStepValue() : nullptr;
        if (singleStepValue == nullptr && stepSCEV != nullptr) {
          if (auto *constant = dyn_cast<llvm::SCEVConstant>(stepSCEV)) {
            singleStepValue = constant->getValue();
          } else if (auto *unknown = dyn_cast<llvm::SCEVUnknown>(stepSCEV)) {
            auto *value = unknown->getValue();
            if (loop->isLoopInvariant(value)) {
              singleStepValue = value;
            }
          }
        }
        std::unordered_set<PHINode *> stepPHIs{&phi};
        std::unordered_set<PHINode *> phis;
        std::unordered_set<Instruction *> nonPHIInstructions;
        std::unordered_set<Instruction *> instructions;
        if (sccContainingIV != nullptr) {
          std::queue<LoopDependenceNode *> ivIntermediateValues;
          std::set<Value *> valuesVisited;
          auto *rootNode = sccContainingIV->fetchNode(&phi);
          if (rootNode != nullptr) {
            ivIntermediateValues.push(rootNode);
          }

          while (!ivIntermediateValues.empty()) {
            auto *node = ivIntermediateValues.front();
            ivIntermediateValues.pop();

            auto *value = node->getValue();
            if (valuesVisited.count(value) != 0) {
              continue;
            }
            valuesVisited.insert(value);

            auto *inst = dyn_cast_or_null<Instruction>(value);
            if (inst == nullptr || !loop->isIncluded(inst)) {
              continue;
            }
            instructions.insert(inst);
            if (auto *innerPhi = dyn_cast<PHINode>(inst)) {
              phis.insert(innerPhi);
            } else {
              nonPHIInstructions.insert(inst);
            }

            for (auto *edge : node->getIncomingEdges()) {
              if (edge->getKind() != LoopDependenceEdgeKind::Variable) {
                continue;
              }
              auto *otherNode = edge->getSrc();
              if (otherNode == nullptr) {
                continue;
              }
              auto *otherValue = otherNode->getValue();
              if (!sccContainingIV->isInternal(otherValue)) {
                continue;
              }
              ivIntermediateValues.push(otherNode);
            }
          }
        }
        if (instructions.empty()) {
          instructions.insert(&phi);
          phis.insert(&phi);
          for (unsigned i = 0; i < phi.getNumIncomingValues(); ++i) {
            auto *incomingBB = phi.getIncomingBlock(i);
            if (!loop->isIncluded(incomingBB)) {
              continue;
            }
            if (auto *inst = dyn_cast<Instruction>(phi.getIncomingValue(i))) {
              instructions.insert(inst);
              if (auto *innerPhi = dyn_cast<PHINode>(inst)) {
                phis.insert(innerPhi);
              } else {
                nonPHIInstructions.insert(inst);
              }
            }
          }
        }
        std::vector<Instruction *> worklist(instructions.begin(),
                                            instructions.end());
        while (!worklist.empty()) {
          auto *current = worklist.back();
          worklist.pop_back();
          for (auto *user : current->users()) {
            auto *userInst = dyn_cast<Instruction>(user);
            if (userInst == nullptr || !loop->isIncluded(userInst) ||
                !isa<CastInst>(userInst)) {
              continue;
            }
            if (instructions.insert(userInst).second) {
              nonPHIInstructions.insert(userInst);
              worklist.push_back(userInst);
            }
          }
        }

        std::unordered_set<Instruction *> derivedInstructions;
        std::queue<Instruction *> derivedWorklist;
        std::unordered_set<Instruction *> visited;
        for (auto *inst : instructions) {
          derivedWorklist.push(inst);
          visited.insert(inst);
        }
        while (!derivedWorklist.empty()) {
          auto *inst = derivedWorklist.front();
          derivedWorklist.pop();
          for (auto *user : inst->users()) {
            auto *userInst = dyn_cast<Instruction>(user);
            if (userInst == nullptr || visited.count(userInst) != 0) {
              continue;
            }
            visited.insert(userInst);
            if (!loop->isIncluded(userInst) ||
                !SE.isSCEVable(userInst->getType())) {
              continue;
            }
            auto *scev = SE.getSCEV(userInst);
            bool supportedSCEV = isa<llvm::SCEVCastExpr>(scev) ||
                                 isa<llvm::SCEVNAryExpr>(scev) ||
                                 isa<llvm::SCEVUDivExpr>(scev);
            if (!supportedSCEV && !userInst->isBinaryOp()) {
              continue;
            }

            bool usesAtLeastOneIVInstruction = false;
            bool valid = true;
            for (auto &operandUse : userInst->operands()) {
              auto *operand = operandUse.get();
              if (isa<ConstantInt>(operand) || IVM.isLoopInvariant(operand)) {
                continue;
              }
              auto *operandInst = dyn_cast<Instruction>(operand);
              if (operandInst == nullptr) {
                valid = false;
                break;
              }
              if (!loop->isIncluded(operandInst)) {
                continue;
              }
              if (instructions.count(operandInst) != 0 ||
                  derivedInstructions.count(operandInst) != 0) {
                usesAtLeastOneIVInstruction = true;
                continue;
              }
              valid = false;
              break;
            }

            if (!valid || !usesAtLeastOneIVInstruction) {
              continue;
            }
            derivedInstructions.insert(userInst);
            derivedWorklist.push(userInst);
          }
        }

        if (stepSCEV == nullptr && !stepPHIs.empty()) {
          auto *stepSCEVPHI = *stepPHIs.begin();
          if (SE.getSCEV(stepSCEVPHI)->getSCEVType() ==
              llvm::SCEVTypes::scAddRecExpr) {
            stepSCEV = cast<llvm::SCEVAddRecExpr>(SE.getSCEV(stepSCEVPHI))
                           ->getStepRecurrence(SE);
            if (singleStepValue == nullptr) {
              if (auto *constant = dyn_cast<llvm::SCEVConstant>(stepSCEV)) {
                singleStepValue = constant->getValue();
              } else if (auto *unknown =
                             dyn_cast<llvm::SCEVUnknown>(stepSCEV)) {
                auto *value = unknown->getValue();
                if (loop->isLoopInvariant(value)) {
                  singleStepValue = value;
                }
              }
            }
          }
        }
        if (startValue != nullptr && singleStepValue != nullptr &&
            stepSCEV != nullptr) {
          IV.reset(new InductionVariable(loop, sccContainingIV, &phi,
                                         startValue, stepSCEV, singleStepValue,
                                         stepPHIs, phis, nonPHIInstructions,
                                          instructions, derivedInstructions));
        }
      }

      if (!IV && noelleDeterminedValidIV && sccContainingIV != nullptr) {
        IV.reset(new InductionVariable(
            loop, IVM, SE, 1, &phi, std::unordered_set<PHINode *>({&phi}),
            sccContainingIV, loopEnvironment, referentialExpander));
      } else if (!IV && llvmDeterminedValidIV && sccContainingIV != nullptr) {
        IV.reset(new InductionVariable(loop, IVM, SE, &phi, sccContainingIV,
                                       loopEnvironment, referentialExpander,
                                       ID));
      }

      if (!IV || !IV->getStepSCEV()) {
        continue;
      }

      owned.push_back(std::move(IV));
    }

    for (auto &iv : owned) {
      auto *ivSCC = iv->getSCC();
      if (ivSCC == nullptr) {
        continue;
      }
      auto candidate = std::unique_ptr<LoopGoverningInductionVariable>(
          new LoopGoverningInductionVariable(
              loop, *iv, *ivSCC, loop->getLoopExitBasicBlocks()));
      if (!candidate->isSCCContainingIVWellFormed()) {
        continue;
      }
      this->governingIVs[loop] = candidate.get();
      this->ownedGoverningIVs.push_back(std::move(candidate));
    }
  }
}

std::unordered_set<InductionVariable *>
InductionVariableManager::getInductionVariables(void) const {
  auto *root = this->loop->getLoop();
  return this->getInductionVariables(*root);
}

std::unordered_set<InductionVariable *>
InductionVariableManager::getInductionVariables(Instruction *i) const {
  std::unordered_set<InductionVariable *> result;
  if (i == nullptr) {
    return result;
  }

  for (auto const &loopIVPair : this->ownedIVs) {
    for (auto const &ownedIV : loopIVPair.second) {
      auto *iv = ownedIV.get();
      if (iv != nullptr && iv->isIVInstruction(i)) {
        result.insert(iv);
      }
    }
  }
  return result;
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

InductionVariable *InductionVariableManager::getInductionVariable(
    LoopStructure &loop, Instruction *i) const {
  if (i == nullptr) {
    return nullptr;
  }
  for (auto *iv : this->getInductionVariables(loop)) {
    if (iv != nullptr && iv->isIVInstruction(i)) {
      return iv;
    }
  }
  return nullptr;
}

bool InductionVariableManager::doesContributeToComputeAnInductionVariable(
    Instruction *i) const {
  return !this->getInductionVariables(i).empty();
}

InductionVariable *InductionVariableManager::getDerivingInductionVariable(
    LoopStructure &loop, Instruction *derivedInstruction) const {
  if (derivedInstruction == nullptr) {
    return nullptr;
  }
  for (auto *iv : this->getInductionVariables(loop)) {
    if (iv != nullptr && iv->isDerivedFromIVInstructions(derivedInstruction)) {
      return iv;
    }
  }
  return nullptr;
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
