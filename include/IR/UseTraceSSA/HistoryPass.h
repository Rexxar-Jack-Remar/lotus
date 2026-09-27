#ifndef LOTUS_IR_USETRACESSA_PASS_H
#define LOTUS_IR_USETRACESSA_PASS_H

#include "IR/UseTraceSSA/LLVMHistory.h"
#include <llvm/IR/PassManager.h>
#include <llvm/Pass.h>
#include <memory>

namespace llvm {
class raw_ostream;
} // namespace llvm

namespace lotus {
namespace usetracessa {

class UseTraceSSAAnalysis : public llvm::AnalysisInfoMixin<UseTraceSSAAnalysis> {
  friend llvm::AnalysisInfoMixin<UseTraceSSAAnalysis>;
  static llvm::AnalysisKey Key;

public:
  /// Deliberately no CFG-only invalidation exemption: uses can change while
  /// the CFG remains identical. Default FAM invalidation is appropriate.
  using Result = LLVMHistoryResult;
  Result run(llvm::Function &function, llvm::FunctionAnalysisManager &);
};

class UseTraceSSAPrinterPass : public llvm::PassInfoMixin<UseTraceSSAPrinterPass> {
public:
  explicit UseTraceSSAPrinterPass(llvm::raw_ostream &out, bool dot = false)
      : Out(out), Dot(dot) {}
  llvm::PreservedAnalyses run(llvm::Function &function,
                              llvm::FunctionAnalysisManager &manager);
private:
  llvm::raw_ostream &Out;
  bool Dot;
};

/// Legacy analysis pass: getAnalysis<UseTraceSSALegacyPass>().getResult().
class UseTraceSSALegacyPass : public llvm::FunctionPass {
public:
  static char ID;
  UseTraceSSALegacyPass();
  bool runOnFunction(llvm::Function &function) override;
  void releaseMemory() override;
  void getAnalysisUsage(llvm::AnalysisUsage &usage) const override;
  const LLVMHistoryResult &getResult() const;
private:
  std::unique_ptr<LLVMHistoryResult> Result;
};

void registerUseTraceSSAAnalysis(llvm::FunctionAnalysisManager &manager);

} // namespace usetracessa
} // namespace lotus
#endif
