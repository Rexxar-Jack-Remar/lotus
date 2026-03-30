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
    llvm::LoopInfo &LI, LoopSCCDAG &sccdag, LoopEnvironment &loopEnvironment)
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
    if (header == nullptr || preHeader == nullptr) {
      continue;
    }

    auto *llvmLoop = LI.getLoopFor(header);
    for (auto &phi : header->phis()) {
      llvm::InductionDescriptor ID{};
      bool llvmDeterminedValidIV = false;
      bool llvmLoopValidForInductionAnalysis =
          (phi.getBasicBlockIndex(preHeader) >= 0) && (llvmLoop != nullptr);
      if (llvmLoopValidForInductionAnalysis &&
          llvm::InductionDescriptor::isInductionPHI(&phi, llvmLoop, &SE, ID)) {
        llvmDeterminedValidIV = true;
      } else if (phi.getType()->isFloatingPointTy() &&
                 llvmLoopValidForInductionAnalysis &&
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

          if (auto *subloopExitBr =
                  dyn_cast<BranchInst>(subloopHeader->getTerminator())) {
            auto *subloopExitBrCondition = subloopExitBr->getCondition();
            if (!isa<CmpInst>(subloopExitBrCondition)) {
              goto allocate_iv;
            }
            auto *subloopExitCond = cast<CmpInst>(subloopExitBrCondition);
            auto *subloopExitCondL = subloopExitCond->getOperand(0);
            auto *subloopExitCondR = subloopExitCond->getOperand(1);

            const llvm::SCEV *subloopIV = nullptr;
            const llvm::SCEV *subloopExitSCEV = nullptr;
            if (SE.getSCEV(subloopExitCondL)->getSCEVType() ==
                    llvm::SCEVTypes::scAddRecExpr &&
                SE.getSCEV(subloopExitCondR)->getSCEVType() ==
                    llvm::SCEVTypes::scConstant) {
              subloopIV = SE.getSCEV(subloopExitCondL);
              subloopExitSCEV = SE.getSCEV(subloopExitCondR);
            } else if (SE.getSCEV(subloopExitCondR)->getSCEVType() ==
                           llvm::SCEVTypes::scAddRecExpr &&
                       SE.getSCEV(subloopExitCondL)->getSCEVType() ==
                           llvm::SCEVTypes::scConstant) {
              subloopIV = SE.getSCEV(subloopExitCondR);
              subloopExitSCEV = SE.getSCEV(subloopExitCondL);
            }

            if (subloopExitSCEV == nullptr || subloopIV == nullptr) {
              goto allocate_iv;
            }

            auto subloopExitConstant = cast<llvm::SCEVConstant>(subloopExitSCEV)
                                           ->getValue()
                                           ->getSExtValue();
            auto *subloopIVSCEV = cast<llvm::SCEVAddRecExpr>(subloopIV);

            auto subloopExitBBs = subloop->getLoopExitBasicBlocks();
            bool exitsOnTrue =
                std::find(subloopExitBBs.begin(), subloopExitBBs.end(),
                          subloopExitBr->getSuccessor(0)) !=
                subloopExitBBs.end();

            if (auto *startSCEVConstant =
                    dyn_cast<llvm::SCEVConstant>(subloopIVSCEV->getStart())) {
              auto subloopStartValue =
                  startSCEVConstant->getValue()->getSExtValue();
              if (auto *stepSCEVConstant = dyn_cast<llvm::SCEVConstant>(
                      subloopIVSCEV->getStepRecurrence(SE))) {
                auto subloopStepSize =
                    stepSCEVConstant->getValue()->getSExtValue();
                auto negativeStep = stepSCEVConstant->getValue()->isNegative();
                bool unhandledCmp = false;
                switch (subloopExitCond->getPredicate()) {
                case CmpInst::Predicate::ICMP_EQ:
                  if (!exitsOnTrue) {
                    unhandledCmp = true;
                  }
                  break;
                case CmpInst::Predicate::ICMP_NE:
                  if (exitsOnTrue) {
                    unhandledCmp = true;
                  }
                  break;
                case CmpInst::Predicate::ICMP_UGT:
                case CmpInst::Predicate::ICMP_SGT:
                  if (negativeStep == exitsOnTrue) {
                    unhandledCmp = true;
                  }
                  if (!negativeStep) {
                    subloopExitConstant += 1;
                  }
                  break;
                case CmpInst::Predicate::ICMP_SGE:
                case CmpInst::Predicate::ICMP_UGE:
                  if (negativeStep == exitsOnTrue) {
                    unhandledCmp = true;
                  }
                  if (negativeStep) {
                    subloopExitConstant += 1;
                  }
                  break;
                case CmpInst::Predicate::ICMP_SLT:
                case CmpInst::Predicate::ICMP_ULT:
                  if (negativeStep != exitsOnTrue) {
                    unhandledCmp = true;
                  }
                  if (negativeStep) {
                    subloopExitConstant += 1;
                  }
                  break;
                case CmpInst::Predicate::ICMP_SLE:
                case CmpInst::Predicate::ICMP_ULE:
                  if (negativeStep != exitsOnTrue) {
                    unhandledCmp = true;
                  }
                  if (!negativeStep) {
                    subloopExitConstant += 1;
                  }
                  break;
                default:
                  unhandledCmp = true;
                  break;
                }

                if (!unhandledCmp) {
                  auto d = std::div(subloopExitConstant - subloopStartValue,
                                    subloopStepSize);
                  stepMultiplier = d.quot + (d.rem ? 1 : 0);
                  IV.reset(new InductionVariable(
                      loop, IVM, SE, stepMultiplier, &phi,
                      std::unordered_set<PHINode *>({internalPHI}),
                      sccContainingIV, loopEnvironment, referentialExpander));
                }
              }
            }
          }
        }
      }

    allocate_iv:
      if (!IV && (noelleDeterminedValidIV || llvmDeterminedValidIV)) {
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

      if (!IV || !IV->getStepSCEV()) {
        continue;
      }

      owned.push_back(std::move(IV));
    }

    for (auto &iv : owned) {
      auto candidate = std::unique_ptr<LoopGoverningInductionVariable>(
          new LoopGoverningInductionVariable(loop, *iv));
      if (!candidate->isSCCContainingIVWellFormed()) {
        continue;
      }
      this->governingIVs[loop] = candidate.get();
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
