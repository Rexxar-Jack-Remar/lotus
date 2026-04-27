#pragma once

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Module.h"

namespace lotus {
namespace analysis {
namespace purity {

enum class PurityKind {
  Pure = 0,
  ReadOnly = 1,
  Impure = 2,
};

llvm::StringRef toString(PurityKind kind);

class FunctionPurityAnalysis {
public:
  explicit FunctionPurityAnalysis(llvm::Module &module);

  void run();

  PurityKind getPurity(const llvm::Function *function) const;
  PurityKind getCallPurity(const llvm::CallBase &call) const;

  bool isPure(const llvm::Function *function) const;
  bool isReadOnly(const llvm::Function *function) const;
  bool isAtMostReadOnly(const llvm::Function *function) const;

private:
  llvm::Module &module_;
  llvm::DenseMap<const llvm::Function *, PurityKind> summaries_;
  bool ran_ = false;

  PurityKind analyzeFunction(const llvm::Function &function) const;
  PurityKind classifyDeclaration(const llvm::Function &function) const;
  PurityKind classifyCall(const llvm::CallBase &call) const;
  PurityKind classifyIntrinsic(const llvm::Function &function) const;

  static PurityKind merge(PurityKind lhs, PurityKind rhs);
};

} // namespace purity
} // namespace analysis
} // namespace lotus
