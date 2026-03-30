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
  explicit ClonableMemoryObject(AllocaInst *allocation);

  AllocaInst *getAllocation(void) const;
  bool isClonableLocation(void) const;
  bool isInstructionCastOrGEPOfLocation(Instruction *I) const;
  bool isInstructionStoringLocation(Instruction *I) const;
  bool isInstructionLoadingLocation(Instruction *I) const;
  void addPointer(Instruction *I);
  void addStore(Instruction *I);
  void addLoad(Instruction *I);
  void setClonable(bool clonable);

private:
  AllocaInst *allocation;
  bool clonable;
  std::unordered_set<Instruction *> castsAndGEPs;
  std::unordered_set<Instruction *> storingInstructions;
  std::unordered_set<Instruction *> loadInstructions;
};

class MemoryCloningAnalysis {
public:
  MemoryCloningAnalysis(LoopStructure *loop, LoopDependenceGraph *ldg);

  std::unordered_set<ClonableMemoryObject *> getClonableMemoryObjects(void) const;
  std::unordered_set<ClonableMemoryObject *> getClonableMemoryObjectsFor(
      Instruction *I) const;

private:
  std::vector<std::unique_ptr<ClonableMemoryObject>> clonableMemoryLocations;
};

} // namespace loop
} // namespace analysis
} // namespace lotus

#endif
