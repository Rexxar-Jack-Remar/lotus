#ifndef LOTUS_ANALYSIS_LOOPINVARIANTS_FUNCTIONINVARIANTANALYSIS_H
#define LOTUS_ANALYSIS_LOOPINVARIANTS_FUNCTIONINVARIANTANALYSIS_H

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/PassManager.h"

#include "Solvers/SMT/LIBSMT/Z3Expr.h"

#include <memory>
#include <string>

namespace llvm {
class DominatorTree;
} // namespace llvm

namespace lotus {

struct FunctionInvariant {
  enum Kind {
    ReturnBound,
    ReturnEquality,
    ReturnNonNegative,
    ReturnComparison,
    Unknown
  };

  Kind InvKind;
  Z3Expr Formula;
  std::string Description;
  llvm::SmallVector<const llvm::Value *, 4> InvolvedValues;

  FunctionInvariant(Kind K, const Z3Expr &F, const std::string &Text = "")
      : InvKind(K), Formula(F), Description(Text) {}

  static const char *getKindName(Kind K);
};

struct FunctionInvariantSet {
  const llvm::Function *Func;
  llvm::SmallVector<FunctionInvariant, 8> Invariants;

  explicit FunctionInvariantSet(const llvm::Function *F) : Func(F) {}

  void addInvariant(FunctionInvariant::Kind K, const Z3Expr &F,
                    const std::string &Text = "") {
    Invariants.emplace_back(K, F, Text);
  }

  bool empty() const { return Invariants.empty(); }
  size_t size() const { return Invariants.size(); }
};

class FunctionInvariantAnalysisResult {
  llvm::DenseMap<const llvm::Function *, std::unique_ptr<FunctionInvariantSet>>
      InvariantSets;

public:
  FunctionInvariantAnalysisResult() = default;

  FunctionInvariantAnalysisResult(const FunctionInvariantAnalysisResult &) =
      delete;
  FunctionInvariantAnalysisResult &
  operator=(const FunctionInvariantAnalysisResult &) = delete;
  FunctionInvariantAnalysisResult(FunctionInvariantAnalysisResult &&) = default;
  FunctionInvariantAnalysisResult &
  operator=(FunctionInvariantAnalysisResult &&) = default;

  const FunctionInvariantSet *getInvariants(const llvm::Function *F) const {
    auto It = InvariantSets.find(F);
    return It != InvariantSets.end() ? It->second.get() : nullptr;
  }

  void setInvariants(const llvm::Function *F,
                     std::unique_ptr<FunctionInvariantSet> Set) {
    InvariantSets[F] = std::move(Set);
  }

  void print(llvm::raw_ostream &OS) const;
};

class FunctionInvariantAnalysis
    : public llvm::AnalysisInfoMixin<FunctionInvariantAnalysis> {
  friend llvm::AnalysisInfoMixin<FunctionInvariantAnalysis>;
  static llvm::AnalysisKey Key;

public:
  using Result = FunctionInvariantAnalysisResult;

  Result run(llvm::Function &F, llvm::FunctionAnalysisManager &AM);
};

class FunctionInvariantPrinterPass
    : public llvm::PassInfoMixin<FunctionInvariantPrinterPass> {
  llvm::raw_ostream &OS;

public:
  explicit FunctionInvariantPrinterPass(llvm::raw_ostream &OS) : OS(OS) {}

  llvm::PreservedAnalyses run(llvm::Function &F,
                              llvm::FunctionAnalysisManager &AM);
};

} // namespace lotus

#endif // LOTUS_ANALYSIS_LOOPINVARIANTS_FUNCTIONINVARIANTANALYSIS_H
