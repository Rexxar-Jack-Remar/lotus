#ifndef LOTUS_ANALYSIS_LOOPINVARIANTS_FUNCTIONINVARIANTCANDIDATEGENERATOR_H
#define LOTUS_ANALYSIS_LOOPINVARIANTS_FUNCTIONINVARIANTCANDIDATEGENERATOR_H

#include "llvm/ADT/SmallVector.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Value.h"

#include "Solvers/SMT/LIBSMT/Z3Expr.h"

#include <string>

namespace llvm {
class BasicBlock;
class ReturnInst;
class BinaryOperator;
} // namespace llvm

namespace lotus {

struct FunctionInvariantCandidate {
  enum CandidateKind {
    ReturnBound,
    ReturnEquality,
    ReturnNonNegative,
    ReturnComparison,
    ReturnPlusComponents,
    ReturnMinusNonNegative,
    Unknown
  };

  CandidateKind Kind;
  llvm::SmallVector<const llvm::Value *, 4> InvolvedValues;
  Z3Expr Formula;
  std::string Description;

  FunctionInvariantCandidate(CandidateKind K) : Kind(K) {}
};

class FunctionInvariantCandidateGenerator {
  const llvm::Function &Func;
  llvm::ScalarEvolution &SE;

  llvm::SmallVector<const llvm::ReturnInst *, 8> ReturnInsts;
  llvm::SmallVector<const llvm::Value *, 8> ReturnValues;

public:
  FunctionInvariantCandidateGenerator(const llvm::Function &F,
                                      llvm::ScalarEvolution &SE);

  void generateCandidates(
      llvm::SmallVectorImpl<FunctionInvariantCandidate> &Candidates);

private:
  void collectReturnValues();

  void generateReturnBoundInvariants(
      llvm::SmallVectorImpl<FunctionInvariantCandidate> &Candidates);

  void generateReturnEqualityInvariants(
      llvm::SmallVectorImpl<FunctionInvariantCandidate> &Candidates);

  void generateReturnNonNegativeInvariants(
      llvm::SmallVectorImpl<FunctionInvariantCandidate> &Candidates);

  void generateReturnComparisonInvariants(
      llvm::SmallVectorImpl<FunctionInvariantCandidate> &Candidates);

  void generateReturnPlusComponentInvariants(
      llvm::SmallVectorImpl<FunctionInvariantCandidate> &Candidates);

  void generateReturnMinusNonNegativeInvariants(
      llvm::SmallVectorImpl<FunctionInvariantCandidate> &Candidates);

  Z3Expr valueToZ3Expr(const llvm::Value *V);
  Z3Expr scevToZ3Expr(const llvm::SCEV *S);
};

} // namespace lotus

#endif // LOTUS_ANALYSIS_LOOPINVARIANTS_FUNCTIONINVARIANTCANDIDATEGENERATOR_H
