#ifndef LOTUS_ANALYSIS_LOOPINVARIANTS_INVARIANTPROVER_H
#define LOTUS_ANALYSIS_LOOPINVARIANTS_INVARIANTPROVER_H

#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/ScalarEvolution.h"

#include "InvariantCandidateGenerator.h"
#include "Solvers/SMT/LIBSMT/Z3Expr.h"

#include <string>
#include <vector>

#include <z3++.h>

namespace llvm {
class DominatorTree;
} // namespace llvm

namespace lotus {

class InvariantProver {
  const llvm::Loop &L;
  llvm::ScalarEvolution &SE;
  llvm::DominatorTree &DT;

  z3::context *Ctx;

  void buildBaseCaseConstraints(z3::solver &Solver);
  void buildStepCaseConstraints(z3::solver &Solver);
  std::string getValueName(const llvm::Value *V);
  z3::expr getInitialValue(const llvm::PHINode *Phi);
  z3::expr getStepExpr(const llvm::PHINode *Phi);
  z3::expr scevToZ3Expr(const llvm::SCEV *S);
  z3::expr renameForNextState(const InvariantCandidate &Candidate,
                              const z3::expr &Invariant);

public:
  InvariantProver(const llvm::Loop &Loop, llvm::ScalarEvolution &SE,
                  llvm::DominatorTree &DT);

  ~InvariantProver();

  struct ProofResult {
    bool IsProven;
    std::string FailureReason;

    ProofResult() : IsProven(false) {}
    explicit ProofResult(bool Proven) : IsProven(Proven) {}
    ProofResult(bool Proven, const std::string &Reason)
        : IsProven(Proven), FailureReason(Reason) {}
  };

  ProofResult proveInvariant(const InvariantCandidate &Candidate);

private:
  ProofResult proveBase(const z3::expr &Invariant, z3::solver &Solver);
  ProofResult proveStep(const z3::expr &InvariantNext,
                        const z3::expr &InvariantCurrent, z3::solver &Solver);
};

} // namespace lotus

#endif // LOTUS_ANALYSIS_LOOPINVARIANTS_INVARIANTPROVER_H
