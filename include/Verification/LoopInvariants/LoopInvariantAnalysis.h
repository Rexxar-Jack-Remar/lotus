//===- LoopInvariantAnalysis.h - Loop Invariant Inference ------*- C++ -*-===//
//
// Part of the Lotus Project
//
//===----------------------------------------------------------------------===//
///
/// \file
/// This file defines the LoopInvariantAnalysis pass which infers loop
/// invariants using a guess-and-check approach based on SCEV analysis and
/// SMT solving.
///
/// The implementation is inspired by xgill's loop invariant inference but
/// adapted to work with LLVM IR and ScalarEvolution instead of custom IR.
///
//===----------------------------------------------------------------------===//

#ifndef LOTUS_ANALYSIS_LOOPINVARIANTS_LOOPINVARIANTANALYSIS_H
#define LOTUS_ANALYSIS_LOOPINVARIANTS_LOOPINVARIANTANALYSIS_H

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/IR/PassManager.h"

#include "Solvers/SMT/LIBSMT/Z3Expr.h"

#include <memory>
#include <string>

namespace llvm {
class Loop;
class Value;
class DominatorTree;
} // namespace llvm

namespace lotus {

/// Represents a single loop invariant (a proven property)
struct LoopInvariant {
  enum Kind {
    Monotonic,      // x >= x_initial or x <= x_initial
    Bound,          // x < n or x <= n from loop exit conditions
    LinearRelation, // (x - x0) * dy == (y - y0) * dx
    Unknown
  };

  Kind InvKind;
  Z3Expr Formula; // The Z3 expression representing the invariant
  llvm::SmallVector<const llvm::Value *, 4> InvolvedValues; // For debugging
  std::string DebugText; // Human-readable description

  LoopInvariant(Kind K, const Z3Expr &F, const std::string &Text = "")
      : InvKind(K), Formula(F), DebugText(Text) {}

  static const char *getKindName(Kind K);
};

/// Set of invariants for a single loop
struct LoopInvariantSet {
  const llvm::Loop *L;
  llvm::SmallVector<LoopInvariant, 8> Invariants;

  explicit LoopInvariantSet(const llvm::Loop *Loop) : L(Loop) {}

  void addInvariant(LoopInvariant::Kind K, const Z3Expr &F,
                    const std::string &Text = "") {
    Invariants.emplace_back(K, F, Text);
  }

  bool empty() const { return Invariants.empty(); }
  size_t size() const { return Invariants.size(); }
};

/// Result type for the loop invariant analysis pass
class LoopInvariantAnalysisResult {
  llvm::DenseMap<const llvm::Loop *, std::unique_ptr<LoopInvariantSet>>
      InvariantSets;

public:
  LoopInvariantAnalysisResult() = default;

  // Move-only type
  LoopInvariantAnalysisResult(const LoopInvariantAnalysisResult &) = delete;
  LoopInvariantAnalysisResult &
  operator=(const LoopInvariantAnalysisResult &) = delete;
  LoopInvariantAnalysisResult(LoopInvariantAnalysisResult &&) = default;
  LoopInvariantAnalysisResult &
  operator=(LoopInvariantAnalysisResult &&) = default;

  /// Get invariants for a specific loop (returns nullptr if none found)
  const LoopInvariantSet *getInvariants(const llvm::Loop *L) const {
    auto It = InvariantSets.find(L);
    return It != InvariantSets.end() ? It->second.get() : nullptr;
  }

  /// Store invariants for a loop
  void setInvariants(const llvm::Loop *L,
                     std::unique_ptr<LoopInvariantSet> Set) {
    InvariantSets[L] = std::move(Set);
  }

  /// Print all invariants for debugging
  void print(llvm::raw_ostream &OS) const;
};

/// Analysis pass that infers loop invariants
class LoopInvariantAnalysis
    : public llvm::AnalysisInfoMixin<LoopInvariantAnalysis> {
  friend llvm::AnalysisInfoMixin<LoopInvariantAnalysis>;
  static llvm::AnalysisKey Key;

public:
  using Result = LoopInvariantAnalysisResult;

  Result run(llvm::Function &F, llvm::FunctionAnalysisManager &AM);
};

/// Printer pass for loop invariants
class LoopInvariantPrinterPass
    : public llvm::PassInfoMixin<LoopInvariantPrinterPass> {
  llvm::raw_ostream &OS;

public:
  explicit LoopInvariantPrinterPass(llvm::raw_ostream &OS) : OS(OS) {}

  llvm::PreservedAnalyses run(llvm::Function &F,
                              llvm::FunctionAnalysisManager &AM);
};

} // namespace lotus

#endif // LOTUS_ANALYSIS_LOOPINVARIANTS_LOOPINVARIANTANALYSIS_H
