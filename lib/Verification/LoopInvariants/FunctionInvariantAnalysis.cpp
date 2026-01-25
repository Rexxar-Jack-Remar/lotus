#include "Verification/LoopInvariants/FunctionInvariantAnalysis.h"

#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/IR/Dominators.h"
#include "llvm/Support/raw_ostream.h"

#include "Verification/LoopInvariants/FunctionInvariantCandidateGenerator.h"
#include "Verification/LoopInvariants/FunctionInvariantProver.h"

using namespace llvm;
using namespace lotus;

AnalysisKey FunctionInvariantAnalysis::Key;

void FunctionInvariantAnalysisResult::print(raw_ostream &OS) const {
  for (const auto &Entry : InvariantSets) {
    const Function *F = Entry.first;
    const FunctionInvariantSet *Set = Entry.second.get();

    if (!Set || Set->empty())
      continue;

    OS << "Function: " << F->getName() << "\n";

    for (const auto &Inv : Set->Invariants) {
      OS << "  [" << FunctionInvariant::getKindName(Inv.InvKind) << "] ";

      if (!Inv.Description.empty()) {
        OS << Inv.Description;
      } else {
        OS << Inv.Formula.to_string();
      }

      OS << "\n";
    }
    OS << "\n";
  }
}

const char *lotus::FunctionInvariant::getKindName(Kind K) {
  switch (K) {
  case ReturnBound:
    return "ReturnBound";
  case ReturnEquality:
    return "ReturnEquality";
  case ReturnNonNegative:
    return "ReturnNonNegative";
  case ReturnComparison:
    return "ReturnComparison";
  case Unknown:
    return "Unknown";
  }
  return "Unknown";
}

FunctionInvariantAnalysisResult
FunctionInvariantAnalysis::run(Function &F, FunctionAnalysisManager &AM) {
  FunctionInvariantAnalysisResult Result;

  auto &SE = AM.getResult<ScalarEvolutionAnalysis>(F);

  SmallVector<FunctionInvariantCandidate, 16> Candidates;
  FunctionInvariantCandidateGenerator Generator(F, SE);
  Generator.generateCandidates(Candidates);

  llvm::errs() << "DEBUG: Generated " << Candidates.size()
               << " function invariant candidates for " << F.getName() << "\n";

  if (Candidates.empty())
    return Result;

  auto InvSet = std::make_unique<FunctionInvariantSet>(&F);
  FunctionInvariantProver Prover(F, SE);

  for (const auto &Candidate : Candidates) {
    llvm::errs() << "DEBUG: Testing function candidate: "
                 << Candidate.Description << "\n";
    llvm::errs() << "DEBUG: Formula: " << Candidate.Formula.to_string() << "\n";

    auto ProofResult = Prover.proveInvariant(Candidate);

    llvm::errs() << "DEBUG: Proof result: "
                 << (ProofResult.IsProven ? "PROVEN" : "FAILED") << "\n";
    if (!ProofResult.IsProven && !ProofResult.FailureReason.empty()) {
      llvm::errs() << "DEBUG: Failure reason: " << ProofResult.FailureReason
                   << "\n";
    }

    if (ProofResult.IsProven) {
      FunctionInvariant::Kind InvKind = FunctionInvariant::Unknown;

      switch (Candidate.Kind) {
      case FunctionInvariantCandidate::ReturnBound:
        InvKind = FunctionInvariant::ReturnBound;
        break;
      case FunctionInvariantCandidate::ReturnEquality:
        InvKind = FunctionInvariant::ReturnEquality;
        break;
      case FunctionInvariantCandidate::ReturnNonNegative:
        InvKind = FunctionInvariant::ReturnNonNegative;
        break;
      case FunctionInvariantCandidate::ReturnComparison:
        InvKind = FunctionInvariant::ReturnComparison;
        break;
      default:
        InvKind = FunctionInvariant::Unknown;
        break;
      }

      InvSet->addInvariant(InvKind, Candidate.Formula, Candidate.Description);
    }
  }

  if (!InvSet->empty()) {
    Result.setInvariants(&F, std::move(InvSet));
  }

  return Result;
}

PreservedAnalyses
FunctionInvariantPrinterPass::run(Function &F, FunctionAnalysisManager &AM) {
  auto &Result = AM.getResult<FunctionInvariantAnalysis>(F);

  OS << "Function Invariants for function: " << F.getName() << "\n";
  OS << "==========================================\n";
  Result.print(OS);

  return PreservedAnalyses::all();
}
