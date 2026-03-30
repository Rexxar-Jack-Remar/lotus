/*
 * Copyright 2026 Lotus contributors
 */
#include "Analysis/Loop/SCCDAGAttrs.h"

namespace lotus {
namespace analysis {
namespace loop {

GenericSCC::GenericSCC(SCCKind K, LoopSCC *s, LoopStructure *loop)
    : loop{loop}, scc{s}, kind{K}, hasMemoryDependences{false} {
  for (auto &pair : s->internalNodePairs()) {
    auto *node = pair.second;
    auto *phi = dyn_cast_or_null<PHINode>(node->getValue());
    if (phi != nullptr) {
      this->PHINodes.insert(phi);
    }
    for (auto *edge : node->getOutgoingEdges()) {
      auto *dst = edge->getDst();
      if (dst == nullptr || !dst->isInternal()) {
        continue;
      }
      auto *dstScc = s;
      (void)dstScc;
      if (edge->getKind() == LoopDependenceEdgeKind::Memory) {
        this->hasMemoryDependences = true;
      }
    }
  }
}

LoopSCC *GenericSCC::getSCC(void) const { return this->scc; }
GenericSCC::SCCKind GenericSCC::getKind(void) const { return this->kind; }
bool GenericSCC::doesHaveMemoryDependencesWithin(void) const { return this->hasMemoryDependences; }
std::set<PHINode *> GenericSCC::getPHIs(void) const { return this->PHINodes; }

LoopIterationSCC::LoopIterationSCC(LoopSCC *s, LoopStructure *loop)
    : GenericSCC(LOOP_ITERATION, s, loop) {}

LoopCarriedSCC::LoopCarriedSCC(SCCKind K,
                               LoopSCC *s,
                               LoopStructure *loop,
                               const std::set<LoopDependenceEdge *> &loopCarriedDependences,
                               bool commutative)
    : GenericSCC(K, s, loop), lcDeps{loopCarriedDependences}, commutative{commutative} {}

std::set<LoopDependenceEdge *> LoopCarriedSCC::getLoopCarriedDependences(void) const {
  return this->lcDeps;
}
bool LoopCarriedSCC::isCommutative(void) const { return this->commutative; }

ReductionSCC::ReductionSCC(SCCKind K,
                           LoopSCC *s,
                           LoopStructure *loop,
                           const std::set<LoopDependenceEdge *> &loopCarriedDependences,
                           Value *initialValue,
                           PHINode *accumulator,
                           Value *identity)
    : LoopCarriedSCC(K, s, loop, loopCarriedDependences, true),
      initialValue{initialValue},
      accumulator{accumulator},
      identity{identity} {}
Value *ReductionSCC::getInitialValue(void) const { return this->initialValue; }
Value *ReductionSCC::getIdentityValue(void) const { return this->identity; }
PHINode *ReductionSCC::getPhiThatAccumulatesValuesBetweenLoopIterations(void) const {
  return this->accumulator;
}

BinaryReductionSCC::BinaryReductionSCC(LoopSCC *s,
                                       LoopStructure *loop,
                                       const std::set<LoopDependenceEdge *> &loopCarriedDependences,
                                       LoopCarriedVariable *variable)
    : ReductionSCC(BINARY_REDUCTION,
                   s,
                   loop,
                   loopCarriedDependences,
                   variable->getInitialValue(),
                   variable->getAccumulator(),
                   variable->getIdentityValue()),
      reductionOperation{variable->getReductionOperation()} {}
Instruction::BinaryOps BinaryReductionSCC::getReductionOperation(void) const {
  return this->reductionOperation;
}

RecomputableSCC::RecomputableSCC(SCCKind K,
                                 LoopSCC *s,
                                 LoopStructure *loop,
                                 const std::set<LoopDependenceEdge *> &loopCarriedDependences,
                                 const std::set<Instruction *> &values,
                                 bool commutative)
    : LoopCarriedSCC(K, s, loop, loopCarriedDependences, commutative),
      values{values} {}
std::set<Instruction *> RecomputableSCC::getValuesToPropagateAcrossLoopIterations(void) const {
  return this->values;
}

SingleAccumulatorRecomputableSCC::SingleAccumulatorRecomputableSCC(
    SCCKind K,
    LoopSCC *s,
    LoopStructure *loop,
    const std::set<LoopDependenceEdge *> &loopCarriedDependences,
    PHINode *accumulator)
    : RecomputableSCC(K, s, loop, loopCarriedDependences, {}, true),
      accumulator{accumulator} {}
PHINode *SingleAccumulatorRecomputableSCC::getPhiThatAccumulatesValuesBetweenLoopIterations(
    void) const {
  return this->accumulator;
}

InductionVariableSCC::InductionVariableSCC(
    SCCKind K,
    LoopSCC *s,
    LoopStructure *loop,
    const std::set<LoopDependenceEdge *> &loopCarriedDependences,
    PHINode *accumulator)
    : SingleAccumulatorRecomputableSCC(K, s, loop, loopCarriedDependences, accumulator) {}

LinearInductionVariableSCC::LinearInductionVariableSCC(
    LoopSCC *s,
    LoopStructure *loop,
    const std::set<LoopDependenceEdge *> &loopCarriedDependences,
    const std::set<InductionVariable *> &ivs)
    : InductionVariableSCC(LINEAR_INDUCTION_VARIABLE,
                           s,
                           loop,
                           loopCarriedDependences,
                           ivs.empty() ? nullptr : (*ivs.begin())->getLoopEntryPHI()),
      IVs{ivs} {}
std::set<InductionVariable *> LinearInductionVariableSCC::getIVs(void) const {
  return this->IVs;
}

PeriodicVariableSCC::PeriodicVariableSCC(
    LoopSCC *s,
    LoopStructure *loop,
    const std::set<LoopDependenceEdge *> &loopCarriedDependences,
    Value *initialValue,
    Value *period,
    Value *step,
    PHINode *accumulator)
    : SingleAccumulatorRecomputableSCC(PERIODIC_VARIABLE,
                                       s,
                                       loop,
                                       loopCarriedDependences,
                                       accumulator),
      initialValue{initialValue},
      period{period},
      step{step} {}
Value *PeriodicVariableSCC::getInitialValue(void) const { return this->initialValue; }
Value *PeriodicVariableSCC::getPeriod(void) const { return this->period; }
Value *PeriodicVariableSCC::getStepValue(void) const { return this->step; }

UnknownClosedFormSCC::UnknownClosedFormSCC(
    LoopSCC *s,
    LoopStructure *loop,
    const std::set<LoopDependenceEdge *> &loopCarriedDependences,
    const std::set<Instruction *> &values)
    : RecomputableSCC(UNKNOWN_CLOSED_FORM, s, loop, loopCarriedDependences, values, false) {}

MemoryClonableSCC::MemoryClonableSCC(
    SCCKind K,
    LoopSCC *s,
    LoopStructure *loop,
    const std::set<LoopDependenceEdge *> &loopCarriedDependences)
    : LoopCarriedSCC(K, s, loop, loopCarriedDependences, false) {}

StackObjectClonableSCC::StackObjectClonableSCC(
    LoopSCC *s,
    LoopStructure *loop,
    const std::set<LoopDependenceEdge *> &loopCarriedDependences,
    const std::set<AllocaInst *> &locations)
    : MemoryClonableSCC(STACK_OBJECT_CLONABLE, s, loop, loopCarriedDependences),
      locations{locations} {}
std::set<AllocaInst *> StackObjectClonableSCC::getMemoryLocationsToClone(void) const {
  return this->locations;
}

LoopCarriedUnknownSCC::LoopCarriedUnknownSCC(
    LoopSCC *s,
    LoopStructure *loop,
    const std::set<LoopDependenceEdge *> &loopCarriedDependences)
    : LoopCarriedSCC(LOOP_CARRIED_UNKNOWN, s, loop, loopCarriedDependences, false) {}

SCCDAGAttrs::SCCDAGAttrs(bool enableFloatAsReal,
                         LoopDependenceGraph *loopDG,
                         LoopSCCDAG *loopSCCDAG,
                         LoopTree *loopNode,
                         InductionVariableManager &IV,
                         noelle::DominatorSummary &DS)
    : enableFloatAsReal{enableFloatAsReal},
      loopDG{loopDG},
      sccdag{loopSCCDAG} {
  this->collectLoopCarriedDependencies(loopNode);
  this->memoryCloningAnalysis.reset(
      new MemoryCloningAnalysis(loopNode->getLoop(), DS, loopDG));

  std::set<InductionVariable *> ivs;
  std::set<InductionVariable *> loopGoverningIVs;
  for (auto *loop : loopNode->getLoops()) {
    auto loopIVs = IV.getInductionVariables(*loop);
    ivs.insert(loopIVs.begin(), loopIVs.end());
    auto *giv = IV.getLoopGoverningInductionVariable(*loop);
    if (giv != nullptr) {
      loopGoverningIVs.insert(giv->getInductionVariable());
    }
  }

  for (auto *scc : loopSCCDAG->getSCCs()) {
    std::unique_ptr<GenericSCC> info;
    if (this->checkIfIndependent(scc)) {
      info.reset(new LoopIterationSCC(scc, loopNode->getLoop()));
    } else {
      auto periodic = this->checkIfPeriodic(scc, loopNode);
      if (std::get<0>(periodic)) {
        info.reset(new PeriodicVariableSCC(scc,
                                           loopNode->getLoop(),
                                           this->sccToLoopCarriedDependencies[scc],
                                           std::get<1>(periodic),
                                           std::get<2>(periodic),
                                           std::get<3>(periodic),
                                           std::get<4>(periodic)));
      } else {
        auto ivSet =
            this->checkIfSCCOnlyContainsInductionVariables(scc, loopNode, ivs, loopGoverningIVs);
        if (!ivSet.empty()) {
          info.reset(new LinearInductionVariableSCC(
              scc, loopNode->getLoop(), this->sccToLoopCarriedDependencies[scc], ivSet));
        } else {
          auto *variable = this->checkIfReducible(scc, loopNode);
          if (variable != nullptr) {
            info.reset(new BinaryReductionSCC(
                scc, loopNode->getLoop(), this->sccToLoopCarriedDependencies[scc], variable));
            delete variable;
          } else {
            auto recomputable = this->checkIfRecomputable(scc, loopNode);
            if (!recomputable.empty()) {
              info.reset(new UnknownClosedFormSCC(
                  scc, loopNode->getLoop(), this->sccToLoopCarriedDependencies[scc], recomputable));
            } else {
              auto clonable = this->checkIfClonableByUsingLocalMemory(scc, loopNode);
              if (!clonable.empty()) {
                info.reset(new StackObjectClonableSCC(
                    scc, loopNode->getLoop(), this->sccToLoopCarriedDependencies[scc], clonable));
              } else {
                info.reset(new LoopCarriedUnknownSCC(
                    scc, loopNode->getLoop(), this->sccToLoopCarriedDependencies[scc]));
              }
            }
          }
        }
      }
    }
    this->sccToInfo[scc] = std::move(info);
  }
}

void SCCDAGAttrs::collectLoopCarriedDependencies(LoopTree *loopNode) {
  for (auto *loop : loopNode->getLoops()) {
    auto edges = LoopCarriedDependencies::getLoopCarriedDependenciesForLoop(
        *loop, loopNode, *this->sccdag);
    for (auto *edge : edges) {
      auto *producerScc = this->sccdag->getSCC(edge->getSrc()->getValue());
      auto *consumerScc = this->sccdag->getSCC(edge->getDst()->getValue());
      if (producerScc != nullptr) {
        this->sccToLoopCarriedDependencies[producerScc].insert(edge);
      }
      if (consumerScc != nullptr) {
        this->sccToLoopCarriedDependencies[consumerScc].insert(edge);
      }
    }
  }
}

LoopCarriedVariable *SCCDAGAttrs::checkIfReducible(LoopSCC *scc, LoopTree *loopNode) {
  auto it = this->sccToLoopCarriedDependencies.find(scc);
  if (it == this->sccToLoopCarriedDependencies.end()) {
    return nullptr;
  }
  std::unordered_set<PHINode *> carriedPhis;
  auto *rootHeader = loopNode->getLoop()->getHeader();
  for (auto *dep : it->second) {
    if (dep->getKind() == LoopDependenceEdgeKind::Memory) {
      return nullptr;
    }
    auto *consumer = dyn_cast_or_null<PHINode>(dep->getDst()->getValue());
    if (consumer == nullptr) {
      return nullptr;
    }
    bool inScc = false;
    for (auto *node : scc->getNodes()) {
      if (node->getValue() == consumer) {
        inScc = true;
        break;
      }
    }
    if (inScc && consumer->getParent() == rootHeader) {
      carriedPhis.insert(consumer);
    }
  }
  if (carriedPhis.size() != 1) {
    return nullptr;
  }
  auto *phi = *carriedPhis.begin();
  auto *variable =
      new LoopCarriedVariable(*loopNode->getLoop(), loopNode, *this->loopDG, *this->sccdag, *scc, phi);
  if (!variable->isEvolutionReducibleAcrossLoopIterations()) {
    delete variable;
    return nullptr;
  }
  if (!this->enableFloatAsReal
      && (phi->getType()->isFloatTy() || phi->getType()->isDoubleTy())) {
    delete variable;
    return nullptr;
  }
  return variable;
}

std::tuple<bool, Value *, Value *, Value *, PHINode *> SCCDAGAttrs::checkIfPeriodic(
    LoopSCC *scc,
    LoopTree *loopNode) {
  auto notPeriodic = std::make_tuple(false, nullptr, nullptr, nullptr, nullptr);
  auto it = this->sccToLoopCarriedDependencies.find(scc);
  if (it == this->sccToLoopCarriedDependencies.end()) {
    return notPeriodic;
  }
  if (scc->numberOfInstructions() != 2) {
    return notPeriodic;
  }
  for (auto *dep : it->second) {
    auto *to = dyn_cast_or_null<PHINode>(dep->getDst()->getValue());
    auto *fromInst = dyn_cast_or_null<Instruction>(dep->getSrc()->getValue());
    if (to == nullptr || fromInst == nullptr) {
      continue;
    }
    if (fromInst->getOpcode() == Instruction::Xor
        && isa<ConstantInt>(fromInst->getOperand(1))
        && cast<ConstantInt>(fromInst->getOperand(1))->isOne()) {
      auto *period = ConstantInt::get(Type::getInt64Ty(to->getContext()), 2);
      auto *step = fromInst->getOperand(1);
      auto *initial = to->getIncomingValueForBlock(loopNode->getLoop()->getPreHeader());
      return std::make_tuple(true, initial, period, step, to);
    }
  }
  return notPeriodic;
}

bool SCCDAGAttrs::checkIfIndependent(LoopSCC *scc) {
  return this->sccToLoopCarriedDependencies.find(scc)
         == this->sccToLoopCarriedDependencies.end();
}

std::set<InductionVariable *> SCCDAGAttrs::checkIfSCCOnlyContainsInductionVariables(
    LoopSCC *scc,
    LoopTree *loopNode,
    std::set<InductionVariable *> &IVs,
    std::set<InductionVariable *> &loopGoverningIVs) const {
  std::set<InductionVariable *> contained;
  std::set<Instruction *> containedInsts;
  for (auto *iv : IVs) {
    for (auto *inst : iv->getAllInstructions()) {
      for (auto &pair : scc->internalNodePairs()) {
        if (pair.first == inst) {
          contained.insert(iv);
          auto all = iv->getAllInstructions();
          containedInsts.insert(all.begin(), all.end());
        }
      }
    }
  }
  if (contained.empty()) {
    return {};
  }

  for (auto *iv : contained) {
    if (loopGoverningIVs.count(iv) == 0) {
      continue;
    }
    LoopGoverningInductionVariable attribution(loopNode->getLoop(), *iv);
    if (!attribution.isSCCContainingIVWellFormed()) {
      continue;
    }
    if (auto *cmp = attribution.getHeaderCompareInstructionToComputeExitCondition()) {
      containedInsts.insert(cmp);
    }
    if (auto *br = attribution.getHeaderBrInst()) {
      containedInsts.insert(br);
    }
    if (auto *condInst =
            dyn_cast<Instruction>(attribution.getExitConditionValue())) {
      containedInsts.insert(condInst);
    }
  }

  for (auto &pair : scc->internalNodePairs()) {
    auto *inst = dyn_cast_or_null<Instruction>(pair.first);
    if (inst != nullptr && containedInsts.count(inst) == 0) {
      return {};
    }
  }
  return contained;
}

std::set<Instruction *> SCCDAGAttrs::checkIfRecomputable(
    LoopSCC *scc,
    LoopTree *loopNode) const {
  auto it = this->sccToLoopCarriedDependencies.find(scc);
  if (it == this->sccToLoopCarriedDependencies.end()) {
    return {};
  }
  std::set<Instruction *> values;
  auto *topLoop = loopNode->getLoop();
  for (auto *dep : it->second) {
    if (dep->getKind() == LoopDependenceEdgeKind::Memory) {
      return {};
    }
    auto *src = dyn_cast_or_null<Instruction>(dep->getSrc()->getValue());
    auto *dst = dyn_cast_or_null<Instruction>(dep->getDst()->getValue());
    if (src == nullptr || dst == nullptr) {
      return {};
    }
    if (loopNode->getInnermostLoopThatContains(src) == topLoop
        || loopNode->getInnermostLoopThatContains(dst) == topLoop) {
      return {};
    }
    values.insert(src);
  }
  return values;
}

std::set<AllocaInst *> SCCDAGAttrs::checkIfClonableByUsingLocalMemory(
    LoopSCC *scc,
    LoopTree *) const {
  std::set<AllocaInst *> allocations;
  bool sawMemoryUsingInstruction = false;
  bool sawMutation = false;
  for (auto &pair : scc->internalNodePairs()) {
    auto *inst = dyn_cast_or_null<Instruction>(pair.first);
    if (inst == nullptr) {
      continue;
    }

    bool memoryUsingInstruction = isa<LoadInst>(inst) || isa<StoreInst>(inst);
    if (auto *call = dyn_cast<CallInst>(inst)) {
      auto intrinsic = call->getIntrinsicID();
      memoryUsingInstruction = memoryUsingInstruction
                               || intrinsic == Intrinsic::memcpy
                               || intrinsic == Intrinsic::memmove;
    }
    if (!memoryUsingInstruction) {
      continue;
    }

    auto locs = this->memoryCloningAnalysis->getClonableMemoryObjectsFor(inst);
    if (locs.empty()) {
      return {};
    }

    bool usesClonableLocation = false;
    for (auto *loc : locs) {
      if (!loc->isInstructionLoadingLocation(inst)
          && !loc->isInstructionStoringLocation(inst)) {
        continue;
      }
      usesClonableLocation = true;
      allocations.insert(loc->getAllocation());
    }
    if (!usesClonableLocation) {
      return {};
    }
    sawMemoryUsingInstruction = true;
    sawMutation = sawMutation || isa<StoreInst>(inst) || isa<CallInst>(inst);
  }
  return (!sawMemoryUsingInstruction || !sawMutation || allocations.empty())
             ? std::set<AllocaInst *>{}
                                                             : allocations;
}

GenericSCC *SCCDAGAttrs::getSCCAttrs(LoopSCC *scc) const {
  auto it = this->sccToInfo.find(scc);
  if (it == this->sccToInfo.end()) {
    return nullptr;
  }
  return it->second.get();
}

std::set<LoopCarriedSCC *> SCCDAGAttrs::getSCCsWithLoopCarriedDependencies(void) const {
  std::set<LoopCarriedSCC *> result;
  for (auto &pair : this->sccToLoopCarriedDependencies) {
    auto *attrs = dynamic_cast<LoopCarriedSCC *>(this->getSCCAttrs(pair.first));
    if (attrs != nullptr) {
      result.insert(attrs);
    }
  }
  return result;
}

LoopSCCDAG *SCCDAGAttrs::getSCCDAG(void) const { return this->sccdag; }

} // namespace loop
} // namespace analysis
} // namespace lotus
