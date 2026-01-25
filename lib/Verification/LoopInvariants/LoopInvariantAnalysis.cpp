#include "Verification/LoopInvariants/LoopInvariantAnalysis.h"

#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/IR/Dominators.h"
#include "llvm/Support/raw_ostream.h"

#include "Verification/LoopInvariants/InvariantCandidateGenerator.h"
#include "Verification/LoopInvariants/InvariantProver.h"

using namespace llvm;
using namespace lotus;

AnalysisKey LoopInvariantAnalysis::Key;

void LoopInvariantAnalysisResult::print(raw_ostream &OS) const {
  for (const auto &Entry : InvariantSets) {
    const Loop *L = Entry.first;
    const LoopInvariantSet *Set = Entry.second.get();

    if (!Set || Set->empty())
      continue;

    OS << "Loop at depth " << L->getLoopDepth() << ":\n";

    for (const auto &Inv : Set->Invariants) {
      OS << "  [" << LoopInvariant::getKindName(Inv.InvKind) << "] ";

      if (!Inv.DebugText.empty()) {
        OS << Inv.DebugText;
      } else {
        OS << Inv.Formula.to_string();
      }

      OS << "\n";
    }
    OS << "\n";
  }
}

const char *lotus::LoopInvariant::getKindName(LoopInvariant::Kind K) {
  switch (K) {
  case LoopInvariant::Monotonic:
    return "Monotonic";
  case LoopInvariant::Bound:
    return "Bound";
  case LoopInvariant::LinearRelation:
    return "LinearRelation";
  case LoopInvariant::Unknown:
    return "Unknown";
  }
  return "Unknown";
}

LoopInvariantAnalysisResult
LoopInvariantAnalysis::run(Function &F, FunctionAnalysisManager &AM) {
  LoopInvariantAnalysisResult Result;

  auto &LI = AM.getResult<LoopAnalysis>(F);
  auto &SE = AM.getResult<ScalarEvolutionAnalysis>(F);
  auto &DT = AM.getResult<DominatorTreeAnalysis>(F);

  SmallVector<Loop *, 16> Worklist;
  for (Loop *Top : LI)
    Worklist.push_back(Top);

  while (!Worklist.empty()) {
    Loop *L = Worklist.pop_back_val();
    for (Loop *Sub : L->getSubLoops())
      Worklist.push_back(Sub);

    auto InvSet = std::make_unique<LoopInvariantSet>(L);

    InvariantCandidateGenerator Generator(*L, SE, LI, DT);
    SmallVector<InvariantCandidate, 16> Candidates;
    Generator.generateCandidates(Candidates);

    llvm::errs() << "DEBUG: Generated " << Candidates.size()
                 << " candidates for loop\n";

    if (Candidates.empty())
      continue;

    InvariantProver Prover(*L, SE, DT);

    for (const auto &Candidate : Candidates) {
      llvm::errs() << "DEBUG: Testing candidate: " << Candidate.Description
                   << "\n";
      llvm::errs() << "DEBUG: Formula: " << Candidate.Formula.to_string()
                   << "\n";

      auto ProofResult = Prover.proveInvariant(Candidate);

      llvm::errs() << "DEBUG: Proof result: "
                   << (ProofResult.IsProven ? "PROVEN" : "FAILED") << "\n";
      if (!ProofResult.IsProven && !ProofResult.FailureReason.empty()) {
        llvm::errs() << "DEBUG: Failure reason: " << ProofResult.FailureReason
                     << "\n";
      }

      if (ProofResult.IsProven) {
        LoopInvariant::Kind InvKind = LoopInvariant::Unknown;

        switch (Candidate.Kind) {
        case InvariantCandidate::MonotonicIncreasing:
        case InvariantCandidate::MonotonicDecreasing:
          InvKind = LoopInvariant::Monotonic;
          break;
        case InvariantCandidate::UpperBound:
        case InvariantCandidate::LowerBound:
          InvKind = LoopInvariant::Bound;
          break;
        case InvariantCandidate::LinearRelationship:
          InvKind = LoopInvariant::LinearRelation;
          break;
        default:
          InvKind = LoopInvariant::Unknown;
          break;
        }

        InvSet->addInvariant(InvKind, Candidate.Formula, Candidate.Description);
      }
    }

    if (!InvSet->empty()) {
      Result.setInvariants(L, std::move(InvSet));
    }
  }

  return Result;
}

PreservedAnalyses LoopInvariantPrinterPass::run(Function &F,
                                                FunctionAnalysisManager &AM) {
  auto &Result = AM.getResult<LoopInvariantAnalysis>(F);

  OS << "Loop Invariants for function: " << F.getName() << "\n";
  OS << "==========================================\n";
  Result.print(OS);

  return PreservedAnalyses::all();
}
