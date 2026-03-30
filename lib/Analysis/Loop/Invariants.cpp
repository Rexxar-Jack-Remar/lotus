/*
 * Copyright 2026 Lotus contributors
 */
#include "Analysis/Loop/Invariants.h"

namespace lotus {
namespace analysis {
namespace loop {

namespace {

bool phiIncomingValuesEquivalent(PHINode *phi) {
  if (phi->getNumIncomingValues() == 0) {
    return false;
  }

  Value *first = phi->getIncomingValue(0);
  for (unsigned i = 1; i < phi->getNumIncomingValues(); ++i) {
    if (phi->getIncomingValue(i) != first) {
      return false;
    }
  }

  return true;
}

bool isInstructionPureEnough(Instruction *instruction) {
  if (isa<StoreInst>(instruction) || instruction->isTerminator()) {
    return false;
  }
  if (auto *call = dyn_cast<CallBase>(instruction)) {
    if (!call->onlyReadsMemory() || !call->doesNotThrow()) {
      return false;
    }
  }
  if (instruction->mayHaveSideEffects()) {
    return false;
  }
  return true;
}

} // namespace

InvariantManager::InvariantManager(LoopStructure *loop,
                                   LoopDependenceGraph *loopDG)
    : loop{loop}, loopDG{loopDG} {
  assert(loop != nullptr);
  assert(loopDG != nullptr);

  for (auto *instruction : loop->getInstructions()) {
    if (loop->isLoopInvariant(instruction)) {
      this->invariants.insert(instruction);
    }
  }

  bool changed = true;
  while (changed) {
    changed = false;
    for (auto *node : loopDG->getInternalNodes()) {
      auto *value = node->getValue();
      auto *instruction = dyn_cast_or_null<Instruction>(value);
      if (instruction == nullptr) {
        continue;
      }
      if (this->invariants.count(instruction) != 0) {
        continue;
      }
      if (!isInstructionPureEnough(instruction)) {
        continue;
      }
      if (auto *phi = dyn_cast<PHINode>(instruction)) {
        if (!phiIncomingValuesEquivalent(phi)) {
          continue;
        }
      }

      bool allInputsInvariant = true;
      for (auto *edge : node->getIncomingEdges()) {
        auto *src = edge->getSrc();
        if (src == nullptr) {
          continue;
        }
        if (edge->getKind() == LoopDependenceEdgeKind::Memory) {
          allInputsInvariant = false;
          break;
        }

        auto *srcValue = src->getValue();
        if (srcValue == nullptr) {
          allInputsInvariant = false;
          break;
        }
        if (!this->isLoopInvariant(srcValue)) {
          allInputsInvariant = false;
          break;
        }
      }

      if (!allInputsInvariant) {
        continue;
      }

      this->invariants.insert(instruction);
      changed = true;
    }
  }
}

bool InvariantManager::isLoopInvariant(Value *value) const {
  if (value == nullptr) {
    return false;
  }
  if (!isa<Instruction>(value)) {
    return true;
  }

  auto *instruction = cast<Instruction>(value);
  if (!this->loop->isIncluded(instruction)) {
    return true;
  }

  return this->invariants.count(instruction) != 0;
}

std::unordered_set<Instruction *>
InvariantManager::getLoopInstructionsThatAreLoopInvariants(void) const {
  return this->invariants;
}

} // namespace loop
} // namespace analysis
} // namespace lotus
