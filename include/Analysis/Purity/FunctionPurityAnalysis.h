#pragma once

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Module.h"

#include <memory>

namespace lotus {
namespace analysis {
namespace purity {

enum class PurityKind {
  Const = 0,
  Pure = 1,
  Impure = 2,
  Unknown = 3,
};

enum class MemorySSAMode {
  Disabled = 0,
  UseIfAvailable = 1,
};

llvm::StringRef toString(PurityKind kind);

struct FunctionEffectSummary {
  bool readsReachableMemory = false;
  bool writesReachableMemory = false;
  bool hasObservableSideEffects = false;
  bool hasUnknownEffects = false;
  bool fromMemorySSA = false;

  PurityKind getPurityKind() const;
  bool isConstCandidate() const;
  bool isPureCandidate() const;
};

class MemorySSAPuritySummaryProvider;

class FunctionPurityAnalysis {
public:
  explicit FunctionPurityAnalysis(
      llvm::Module &module,
      MemorySSAMode memorySSAMode = MemorySSAMode::UseIfAvailable);
  ~FunctionPurityAnalysis();

  void run();

  PurityKind getPurity(const llvm::Function *function) const;
  PurityKind getCallPurity(const llvm::CallBase &call) const;
  FunctionEffectSummary getEffects(const llvm::Function *function) const;

  bool hasMemorySSASummaries() const;

  bool isConst(const llvm::Function *function) const;
  bool isPure(const llvm::Function *function) const;
  bool isAtMostPure(const llvm::Function *function) const;
  bool isKnown(const llvm::Function *function) const;

private:
  llvm::Module &module_;
  MemorySSAMode memorySSAMode_;
  llvm::DenseMap<const llvm::Function *, FunctionEffectSummary> summaries_;
  std::unique_ptr<MemorySSAPuritySummaryProvider> memorySSAProvider_;
  bool ran_ = false;

  FunctionEffectSummary initialSummary(const llvm::Function &function) const;
  FunctionEffectSummary analyzeFunction(const llvm::Function &function) const;
  FunctionEffectSummary classifyDeclaration(
      const llvm::Function &function,
      const llvm::CallBase *callSite = nullptr) const;
  FunctionEffectSummary classifyCall(const llvm::CallBase &call) const;
  FunctionEffectSummary classifyIntrinsic(const llvm::Function &function) const;

  static FunctionEffectSummary merge(const FunctionEffectSummary &lhs,
                                     const FunctionEffectSummary &rhs);
};

} // namespace purity
} // namespace analysis
} // namespace lotus
