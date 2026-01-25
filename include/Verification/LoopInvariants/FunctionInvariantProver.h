#ifndef LOTUS_ANALYSIS_LOOPINVARIANTS_FUNCTIONINVARIANTPROVER_H
#define LOTUS_ANALYSIS_LOOPINVARIANTS_FUNCTIONINVARIANTPROVER_H

#include "llvm/Analysis/ScalarEvolution.h"

#include "FunctionInvariantCandidateGenerator.h"
#include "Solvers/SMT/LIBSMT/Z3Expr.h"

#include <string>

#include <z3++.h>

namespace llvm {
class DominatorTree;
class Function;
} // namespace llvm

namespace lotus {

class FunctionInvariantProver {
  const llvm::Function &Func;
  llvm::ScalarEvolution &SE;

  z3::context *Ctx;

  std::string getValueName(const llvm::Value *V);

public:
  FunctionInvariantProver(const llvm::Function &F, llvm::ScalarEvolution &SE);

  ~FunctionInvariantProver();

  struct ProofResult {
    bool IsProven;
    std::string FailureReason;

    ProofResult() : IsProven(false) {}
    explicit ProofResult(bool Proven) : IsProven(Proven) {}
    ProofResult(bool Proven, const std::string &Reason)
        : IsProven(Proven), FailureReason(Reason) {}
  };

  ProofResult proveInvariant(const FunctionInvariantCandidate &Candidate);

private:
  ProofResult proveAtExit(const z3::expr &Invariant, z3::solver &Solver);
};

} // namespace lotus

#endif // LOTUS_ANALYSIS_LOOPINVARIANTS_FUNCTIONINVARIANTPROVER_H
