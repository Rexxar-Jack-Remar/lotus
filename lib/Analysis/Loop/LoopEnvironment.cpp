/*
 * Copyright 2026  Lotus contributors
 */
#include "Analysis/Loop/LoopEnvironment.h"

namespace lotus {
namespace analysis {
namespace loop {

LoopEnvironment::LoopEnvironment(LoopDependenceGraph *loopDG,
                                 std::vector<BasicBlock *> const &exitBlocks)
    : hasExitBlockEnv{exitBlocks.size() > 1}, exitBlockType{nullptr} {
  assert(loopDG != nullptr);

  for (auto *externalNode : loopDG->getExternalNodes()) {
    auto *externalValue = externalNode->getValue();
    if (externalValue == nullptr) {
      continue;
    }

    bool isProducer = false;
    std::unordered_set<Instruction *> consumersOfLiveInValue;
    for (auto *edge : externalNode->getOutgoingEdges()) {
      if (edge->getKind() != LoopDependenceEdgeKind::Variable) {
        continue;
      }
      auto *consumer = dyn_cast_or_null<Instruction>(edge->getDst()->getValue());
      if (consumer == nullptr) {
        continue;
      }
      isProducer = true;
      consumersOfLiveInValue.insert(consumer);
    }
    if (isProducer) {
      this->addLiveInValue(externalValue, consumersOfLiveInValue);
    }

    for (auto *edge : externalNode->getIncomingEdges()) {
      if (edge->getKind() != LoopDependenceEdgeKind::Variable) {
        continue;
      }
      auto *internalValue = edge->getSrc()->getValue();
      if (internalValue == nullptr) {
        continue;
      }
      if (!this->isProducer(internalValue)) {
        this->addLiveOutProducer(internalValue);
      }
      this->prodConsumers[internalValue].insert(externalValue);
    }
  }

  if (this->hasExitBlockEnv) {
    auto &context = exitBlocks.front()->getContext();
    this->exitBlockType = IntegerType::get(context, 32);
  }
}

iterator_range<std::vector<Value *>::iterator> LoopEnvironment::getProducers(void) {
  return make_range(this->envProducers.begin(), this->envProducers.end());
}

iterator_range<std::set<int>::iterator>
LoopEnvironment::getEnvIDsOfLiveInVars(void) const {
  return make_range(this->liveInIDs.begin(), this->liveInIDs.end());
}

iterator_range<std::set<int>::iterator>
LoopEnvironment::getEnvIDsOfLiveOutVars(void) const {
  return make_range(this->liveOutIDs.begin(), this->liveOutIDs.end());
}

uint64_t LoopEnvironment::size(void) const {
  return this->envProducers.size() + (this->hasExitBlockEnv ? 1 : 0);
}

uint64_t LoopEnvironment::getNumberOfLiveIns(void) const {
  return this->liveInIDs.size();
}

uint64_t LoopEnvironment::getNumberOfLiveOuts(void) const {
  return this->liveOutIDs.size();
}

int64_t LoopEnvironment::getExitBlockID(void) const {
  return this->hasExitBlockEnv ? static_cast<int64_t>(this->envProducers.size())
                               : -1;
}

Type *LoopEnvironment::typeOfEnvironmentLocation(uint64_t id) const {
  if (id < this->envProducers.size()) {
    return this->envProducers[id]->getType();
  }
  return this->exitBlockType;
}

std::vector<Type *> LoopEnvironment::getTypesOfEnvironmentLocations(void) const {
  std::vector<Type *> types;
  for (uint64_t i = 0; i < this->size(); ++i) {
    types.push_back(this->typeOfEnvironmentLocation(i));
  }
  return types;
}

bool LoopEnvironment::isLiveIn(Value *val) const {
  auto it = this->producerIDMap.find(val);
  if (it == this->producerIDMap.end()) {
    return false;
  }
  return this->liveInIDs.find(it->second) != this->liveInIDs.end();
}

bool LoopEnvironment::isProducer(Value *producer) const {
  return this->producerIDMap.find(producer) != this->producerIDMap.end();
}

Value *LoopEnvironment::getProducer(uint64_t id) const {
  assert(id < this->envProducers.size());
  return this->envProducers[id];
}

std::set<Value *> LoopEnvironment::consumersOf(Value *prod) const {
  auto it = this->prodConsumers.find(prod);
  if (it == this->prodConsumers.end()) {
    return {};
  }
  return it->second;
}

uint64_t LoopEnvironment::addProducer(Value *producer, bool liveIn) {
  auto existing = this->producerIDMap.find(producer);
  if (existing != this->producerIDMap.end()) {
    return existing->second;
  }

  auto nextID = static_cast<uint64_t>(this->envProducers.size());
  this->envProducers.push_back(producer);
  this->producerIDMap[producer] = static_cast<int>(nextID);
  if (liveIn) {
    this->liveInIDs.insert(static_cast<int>(nextID));
  } else {
    this->liveOutIDs.insert(static_cast<int>(nextID));
  }
  return nextID;
}

uint64_t LoopEnvironment::addLiveInProducer(Value *producer) {
  return this->addProducer(producer, true);
}

void LoopEnvironment::addLiveOutProducer(Value *producer) {
  (void)this->addProducer(producer, false);
}

uint64_t LoopEnvironment::addLiveInValue(
    Value *newLiveInValue,
    const std::unordered_set<Instruction *> &consumers) {
  auto newID = this->addLiveInProducer(newLiveInValue);
  for (auto *consumer : consumers) {
    this->prodConsumers[newLiveInValue].insert(consumer);
  }
  return newID;
}

} // namespace loop
} // namespace analysis
} // namespace lotus
