#ifndef LOTUS_ANALYSIS_LOOPINVARIANTS_INVARIANTCANDIDATEGENERATOR_H
#define LOTUS_ANALYSIS_LOOPINVARIANTS_INVARIANTCANDIDATEGENERATOR_H

#include "llvm/ADT/SmallVector.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/IR/Value.h"

#include "Solvers/SMT/LIBSMT/Z3Expr.h"

#include <string>
#include <vector>

namespace llvm {
class ICmpInst;
class PHINode;
class BranchInst;
class StoreInst;
class LoadInst;
} // namespace llvm

namespace lotus {

struct InductionVariableInfo {
  const llvm::PHINode *Phi;
  const llvm::SCEV *InitialValue;
  const llvm::SCEV *Step;
  bool IsIncreasing;
  bool IsDecreasing;
  bool HasConstantStep;
  int64_t ConstantStep;

  bool IsPointerInduction;
  llvm::Type *PointerElementType;
  uint64_t ElementSize;

  InductionVariableInfo()
      : Phi(nullptr), InitialValue(nullptr), Step(nullptr), IsIncreasing(false),
        IsDecreasing(false), HasConstantStep(false), ConstantStep(0),
        IsPointerInduction(false), PointerElementType(nullptr), ElementSize(0) {
  }
};

struct LoopBoundInfo {
  const llvm::ICmpInst *ExitCond;
  const llvm::Value *BoundValue;
  llvm::CmpInst::Predicate Predicate;
  const llvm::PHINode *InductionVar;

  LoopBoundInfo()
      : ExitCond(nullptr), BoundValue(nullptr),
        Predicate(llvm::CmpInst::BAD_ICMP_PREDICATE), InductionVar(nullptr) {}
};

struct ValueDelta {
  const llvm::Value *Lval;
  int64_t Delta;
  bool HasDelta;
  bool IsPointerStride;
  llvm::Type *StrideType;
  bool MonotonicIncr;
  bool MonotonicDecr;

  ValueDelta()
      : Lval(nullptr), Delta(0), HasDelta(false), IsPointerStride(false),
        StrideType(nullptr), MonotonicIncr(false), MonotonicDecr(false) {}
};

struct LoopAssignment {
  const llvm::Value *Left;
  const llvm::Value *Right;

  LoopAssignment() : Left(nullptr), Right(nullptr) {}
  LoopAssignment(const llvm::Value *L, const llvm::Value *R)
      : Left(L), Right(R) {}
};

struct TerminatorInfo {
  const llvm::Value *Target;
  const llvm::Value *TerminateTest;
  int64_t TerminateInt;

  TerminatorInfo() : Target(nullptr), TerminateTest(nullptr), TerminateInt(0) {}
  TerminatorInfo(const llvm::Value *T, const llvm::Value *Test, int64_t Int)
      : Target(T), TerminateTest(Test), TerminateInt(Int) {}
};

struct LoopComparison {
  const llvm::Value *Source;
  const llvm::Value *Target;
  llvm::CmpInst::Predicate Predicate;
  bool IsPointerComparison;
  llvm::Type *StrideType;

  LoopComparison()
      : Source(nullptr), Target(nullptr),
        Predicate(llvm::CmpInst::BAD_ICMP_PREDICATE),
        IsPointerComparison(false), StrideType(nullptr) {}
};

struct InvariantCandidate {
  enum CandidateKind {
    MonotonicIncreasing,
    MonotonicDecreasing,
    UpperBound,
    LowerBound,
    LinearRelationship,
    AssignmentBased,
    Terminator,
    Implication,
    FlagBased,
    Unknown
  };

  CandidateKind Kind;
  llvm::SmallVector<const llvm::Value *, 4> InvolvedValues;
  Z3Expr Formula;
  std::string Description;

  Z3Expr Premise;
  bool IsImplication;

  InvariantCandidate(CandidateKind K) : Kind(K), IsImplication(false) {}
};

class InvariantCandidateGenerator {
  const llvm::Loop &L;
  llvm::ScalarEvolution &SE;
  llvm::LoopInfo &LI;
  llvm::DominatorTree &DT;

  llvm::SmallVector<InductionVariableInfo, 8> InductionVars;
  llvm::SmallVector<LoopBoundInfo, 4> LoopBounds;
  llvm::SmallVector<ValueDelta, 8> ValueDeltas;
  llvm::SmallVector<LoopAssignment, 16> LoopAssignments;
  llvm::SmallVector<TerminatorInfo, 8> Terminators;
  llvm::SmallVector<LoopComparison, 16> LoopComparisons;

public:
  InvariantCandidateGenerator(const llvm::Loop &Loop, llvm::ScalarEvolution &SE,
                              llvm::LoopInfo &LI, llvm::DominatorTree &DT);

  void
  generateCandidates(llvm::SmallVectorImpl<InvariantCandidate> &Candidates);

private:
  void analyzeInductionVariables();
  void extractLoopBounds();
  void analyzeGEPInstructions();
  void collectAssignments();
  void analyzeValueDeltas();
  void inferTerminators();
  void collectLoopComparisons();

  void generateMonotonicityInvariants(
      llvm::SmallVectorImpl<InvariantCandidate> &Candidates);

  void generateBoundInvariants(
      llvm::SmallVectorImpl<InvariantCandidate> &Candidates);

  void generateLinearRelationInvariants(
      llvm::SmallVectorImpl<InvariantCandidate> &Candidates);

  void generateAssignmentBasedInvariants(
      llvm::SmallVectorImpl<InvariantCandidate> &Candidates);

  void generateTerminatorInvariants(
      llvm::SmallVectorImpl<InvariantCandidate> &Candidates);

  void generateFlagBasedInvariants(
      llvm::SmallVectorImpl<InvariantCandidate> &Candidates);

  void
  tryImplicationWeakening(llvm::SmallVectorImpl<InvariantCandidate> &Candidates,
                          const InvariantCandidate &FailedCandidate);

  Z3Expr scevToZ3Expr(const llvm::SCEV *S);
  Z3Expr valueToZ3Expr(const llvm::Value *V);
  Z3Expr pointerToZ3Expr(const llvm::Value *V, llvm::Type *ElementType);
  Z3Expr getInitialValue(const llvm::Value *V);
  Z3Expr getImplicationPremise(const llvm::Value *V);
};

} // namespace lotus

#endif
