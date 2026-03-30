/*
 * Copyright 2026 Lotus contributors
 */
#ifndef LOTUS_ANALYSIS_LOOP_MEMORYCLONINGANALYSIS_H
#define LOTUS_ANALYSIS_LOOP_MEMORYCLONINGANALYSIS_H

#include "Analysis/Loop/LoopDependenceGraph.h"

namespace lotus {
namespace analysis {
namespace loop {

class ClonableMemoryObject {
public:
  ClonableMemoryObject(AllocaInst *allocation, uint64_t sizeInBits);

  AllocaInst *getAllocation(void) const;
  uint64_t getAllocationSizeInBits(void) const;
  bool isClonableLocation(void) const;
  bool doPrivateCopiesNeedToBeInitialized(void) const;
  bool mustAliasAMemoryLocationWithinObject(Value *pointer) const;
  bool isInstructionCastOrGEPOfLocation(Instruction *I) const;
  bool isInstructionStoringLocation(Instruction *I) const;
  bool isInstructionLoadingLocation(Instruction *I) const;
  bool isInstructionUsingLocationWithoutStoring(Instruction *I) const;
  void addPointer(Instruction *I);
  void addStore(Instruction *I);
  void addLoad(Instruction *I);
  void addNonStoringUse(Instruction *I);
  std::unordered_set<Instruction *> getLocationPointerInstructions(void) const;
  void setClonable(bool clonable);
  void setNeedsInitialization(bool needsInitialization);

private:
  AllocaInst *allocation;
  uint64_t sizeInBits;
  bool clonable;
  bool needsInitialization;
  std::unordered_set<Instruction *> castsAndGEPs;
  std::unordered_set<Instruction *> storingInstructions;
  std::unordered_set<Instruction *> loadInstructions;
  std::unordered_set<Instruction *> nonStoringInstructions;
};

class MemoryCloningAnalysis {
public:
  MemoryCloningAnalysis(LoopStructure *loop, noelle::DominatorSummary &DS,
                        LoopDependenceGraph *ldg);

  std::unordered_set<ClonableMemoryObject *>
  getClonableMemoryObjects(void) const;
  std::unordered_set<ClonableMemoryObject *>
  getClonableMemoryObjectsFor(Instruction *I) const;

private:
  std::vector<std::unique_ptr<ClonableMemoryObject>> clonableMemoryLocations;
};

} // namespace loop
} // namespace analysis
} // namespace lotus

#endif
