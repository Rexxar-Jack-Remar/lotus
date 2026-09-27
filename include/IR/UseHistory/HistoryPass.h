#ifndef LOTUS_IR_USEHISTORY_PASS_H
#define LOTUS_IR_USEHISTORY_PASS_H

#include "IR/UseHistory/LLVMHistory.h"
#include <llvm/IR/PassManager.h>
#include <llvm/Pass.h>
#include <memory>

namespace llvm {
class raw_ostream;
} // namespace llvm

namespace lotus {
namespace usehistory {

class UseHistoryAnalysis : public llvm::AnalysisInfoMixin<UseHistoryAnalysis> {
  friend llvm::AnalysisInfoMixin<UseHistoryAnalysis>;
  static llvm::AnalysisKey Key;

public:
  /// Deliberately no CFG-only invalidation exemption: uses can change while
  /// the CFG remains identical. Default FAM invalidation is appropriate.
  using Result = LLVMHistoryResult;
  Result run(llvm::Function &function, llvm::FunctionAnalysisManager &);
};

class UseHistoryPrinterPass : public llvm::PassInfoMixin<UseHistoryPrinterPass> {
public:
  explicit UseHistoryPrinterPass(llvm::raw_ostream &out, bool dot = false)
      : Out(out), Dot(dot) {}
  llvm::PreservedAnalyses run(llvm::Function &function,
                              llvm::FunctionAnalysisManager &manager);
private:
  llvm::raw_ostream &Out;
  bool Dot;
};

/// Legacy analysis pass: getAnalysis<UseHistoryLegacyPass>().getResult().
class UseHistoryLegacyPass : public llvm::FunctionPass {
public:
  static char ID;
  UseHistoryLegacyPass();
  bool runOnFunction(llvm::Function &function) override;
  void releaseMemory() override;
  void getAnalysisUsage(llvm::AnalysisUsage &usage) const override;
  const LLVMHistoryResult &getResult() const;
private:
  std::unique_ptr<LLVMHistoryResult> Result;
};

void registerUseHistoryAnalysis(llvm::FunctionAnalysisManager &manager);

} // namespace usehistory
} // namespace lotus
#endif
