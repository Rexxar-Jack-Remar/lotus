/*
 * Copyright 2026 Lotus contributors
 */
#ifndef LOTUS_ANALYSIS_LOOP_SCCDAGATTRS_H
#define LOTUS_ANALYSIS_LOOP_SCCDAGATTRS_H

#include "Analysis/Loop/LoopIterationSpaceAnalysis.h"
#include "Analysis/Loop/MemoryCloningAnalysis.h"
#include "Analysis/Loop/Variable.h"

namespace lotus {
namespace analysis {
namespace loop {

class GenericSCC {
public:
  enum SCCKind {
    LOOP_CARRIED,
    REDUCTION,
    BINARY_REDUCTION,
    RECOMPUTABLE,
    SINGLE_ACCUMULATOR_RECOMPUTABLE,
    INDUCTION_VARIABLE,
    LINEAR_INDUCTION_VARIABLE,
    PERIODIC_VARIABLE,
    UNKNOWN_CLOSED_FORM,
    MEMORY_CLONABLE,
    STACK_OBJECT_CLONABLE,
    LOOP_CARRIED_UNKNOWN,
    LOOP_ITERATION
  };

  GenericSCC(SCCKind K, LoopSCC *s, LoopStructure *loop);
  virtual ~GenericSCC() = default;

  LoopSCC *getSCC(void) const;
  SCCKind getKind(void) const;
  bool doesHaveMemoryDependencesWithin(void) const;
  std::set<PHINode *> getPHIs(void) const;

protected:
  LoopStructure *loop;
  LoopSCC *scc;
  std::set<PHINode *> PHINodes;

private:
  SCCKind kind;
  bool hasMemoryDependences;
};

class LoopIterationSCC : public GenericSCC {
public:
  LoopIterationSCC(LoopSCC *s, LoopStructure *loop);
};

class LoopCarriedSCC : public GenericSCC {
public:
  LoopCarriedSCC(SCCKind K,
                 LoopSCC *s,
                 LoopStructure *loop,
                 const std::set<LoopDependenceEdge *> &loopCarriedDependences,
                 bool commutative);

  std::set<LoopDependenceEdge *> getLoopCarriedDependences(void) const;
  bool isCommutative(void) const;

protected:
  std::set<LoopDependenceEdge *> lcDeps;
  bool commutative;
};

class ReductionSCC : public LoopCarriedSCC {
public:
  ReductionSCC(SCCKind K,
               LoopSCC *s,
               LoopStructure *loop,
               const std::set<LoopDependenceEdge *> &loopCarriedDependences,
               Value *initialValue,
               PHINode *accumulator,
               Value *identity);

  Value *getInitialValue(void) const;
  Value *getIdentityValue(void) const;
  PHINode *getPhiThatAccumulatesValuesBetweenLoopIterations(void) const;

protected:
  Value *initialValue;
  PHINode *accumulator;
  Value *identity;
};

class BinaryReductionSCC : public ReductionSCC {
public:
  BinaryReductionSCC(LoopSCC *s,
                     LoopStructure *loop,
                     const std::set<LoopDependenceEdge *> &loopCarriedDependences,
                     LoopCarriedVariable *variable);

  Instruction::BinaryOps getReductionOperation(void) const;

private:
  Instruction::BinaryOps reductionOperation;
};

class RecomputableSCC : public LoopCarriedSCC {
public:
  RecomputableSCC(SCCKind K,
                  LoopSCC *s,
                  LoopStructure *loop,
                  const std::set<LoopDependenceEdge *> &loopCarriedDependences,
                  const std::set<Instruction *> &values,
                  bool commutative);

  std::set<Instruction *> getValuesToPropagateAcrossLoopIterations(void) const;

protected:
  std::set<Instruction *> values;
};

class SingleAccumulatorRecomputableSCC : public RecomputableSCC {
public:
  SingleAccumulatorRecomputableSCC(SCCKind K,
                                   LoopSCC *s,
                                   LoopStructure *loop,
                                   const std::set<LoopDependenceEdge *> &loopCarriedDependences,
                                   PHINode *accumulator);

  PHINode *getPhiThatAccumulatesValuesBetweenLoopIterations(void) const;

protected:
  PHINode *accumulator;
};

class InductionVariableSCC : public SingleAccumulatorRecomputableSCC {
public:
  InductionVariableSCC(SCCKind K,
                       LoopSCC *s,
                       LoopStructure *loop,
                       const std::set<LoopDependenceEdge *> &loopCarriedDependences,
                       PHINode *accumulator);
};

class LinearInductionVariableSCC : public InductionVariableSCC {
public:
  LinearInductionVariableSCC(LoopSCC *s,
                             LoopStructure *loop,
                             const std::set<LoopDependenceEdge *> &loopCarriedDependences,
                             const std::set<InductionVariable *> &ivs);

  std::set<InductionVariable *> getIVs(void) const;

private:
  std::set<InductionVariable *> IVs;
};

class PeriodicVariableSCC : public SingleAccumulatorRecomputableSCC {
public:
  PeriodicVariableSCC(LoopSCC *s,
                      LoopStructure *loop,
                      const std::set<LoopDependenceEdge *> &loopCarriedDependences,
                      Value *initialValue,
                      Value *period,
                      Value *step,
                      PHINode *accumulator);

  Value *getInitialValue(void) const;
  Value *getPeriod(void) const;
  Value *getStepValue(void) const;

private:
  Value *initialValue;
  Value *period;
  Value *step;
};

class UnknownClosedFormSCC : public RecomputableSCC {
public:
  UnknownClosedFormSCC(LoopSCC *s,
                       LoopStructure *loop,
                       const std::set<LoopDependenceEdge *> &loopCarriedDependences,
                       const std::set<Instruction *> &values);
};

class MemoryClonableSCC : public LoopCarriedSCC {
public:
  MemoryClonableSCC(SCCKind K,
                    LoopSCC *s,
                    LoopStructure *loop,
                    const std::set<LoopDependenceEdge *> &loopCarriedDependences);
};

class StackObjectClonableSCC : public MemoryClonableSCC {
public:
  StackObjectClonableSCC(LoopSCC *s,
                         LoopStructure *loop,
                         const std::set<LoopDependenceEdge *> &loopCarriedDependences,
                         const std::set<AllocaInst *> &locations);

  std::set<AllocaInst *> getMemoryLocationsToClone(void) const;

private:
  std::set<AllocaInst *> locations;
};

class LoopCarriedUnknownSCC : public LoopCarriedSCC {
public:
  LoopCarriedUnknownSCC(LoopSCC *s,
                        LoopStructure *loop,
                        const std::set<LoopDependenceEdge *> &loopCarriedDependences);
};

class SCCDAGAttrs {
public:
  SCCDAGAttrs(bool enableFloatAsReal,
              LoopDependenceGraph *loopDG,
              LoopSCCDAG *loopSCCDAG,
              LoopTree *loopNode,
              InductionVariableManager &IV,
              noelle::DominatorSummary &DS);

  GenericSCC *getSCCAttrs(LoopSCC *scc) const;
  std::set<LoopCarriedSCC *> getSCCsWithLoopCarriedDependencies(void) const;
  LoopSCCDAG *getSCCDAG(void) const;

private:
  std::map<LoopSCC *, std::set<LoopDependenceEdge *>> sccToLoopCarriedDependencies;
  bool enableFloatAsReal;
  std::unordered_map<LoopSCC *, std::unique_ptr<GenericSCC>> sccToInfo;
  LoopDependenceGraph *loopDG;
  LoopSCCDAG *sccdag;
  std::unique_ptr<MemoryCloningAnalysis> memoryCloningAnalysis;

  void collectLoopCarriedDependencies(LoopTree *loopNode);
  LoopCarriedVariable *checkIfReducible(LoopSCC *scc, LoopTree *loopNode);
  std::tuple<bool, Value *, Value *, Value *, PHINode *> checkIfPeriodic(
      LoopSCC *scc,
      LoopTree *loopNode);
  bool checkIfIndependent(LoopSCC *scc);
  std::set<InductionVariable *> checkIfSCCOnlyContainsInductionVariables(
      LoopSCC *scc,
      LoopTree *loopNode,
      std::set<InductionVariable *> &IVs,
      std::set<InductionVariable *> &loopGoverningIVs) const;
  std::set<Instruction *> checkIfRecomputable(LoopSCC *scc,
                                              LoopTree *loopNode) const;
  std::set<AllocaInst *> checkIfClonableByUsingLocalMemory(
      LoopSCC *scc,
      LoopTree *loopNode) const;
};

} // namespace loop
} // namespace analysis
} // namespace lotus

#endif
