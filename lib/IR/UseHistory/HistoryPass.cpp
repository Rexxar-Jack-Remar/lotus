#include "IR/UseHistory/HistoryPass.h"
#include <llvm/Support/raw_ostream.h>
#include <sstream>
#include <stdexcept>

namespace lotus {
namespace usehistory {

llvm::AnalysisKey UseHistoryAnalysis::Key;
char UseHistoryLegacyPass::ID = 0;

UseHistoryAnalysis::Result UseHistoryAnalysis::run(llvm::Function &function,
                                    llvm::FunctionAnalysisManager &) {
  return LLVMHistoryBuilder::build(function);
}

llvm::PreservedAnalyses
UseHistoryPrinterPass::run(llvm::Function &function,
                    llvm::FunctionAnalysisManager &manager) {
  const auto &result = manager.getResult<UseHistoryAnalysis>(function);
  std::ostringstream text;
  if (Dot) result.graph().printDOT(text);
  else result.graph().print(text);
  Out << text.str();
  return llvm::PreservedAnalyses::all();
}

UseHistoryLegacyPass::UseHistoryLegacyPass() : llvm::FunctionPass(ID) {}

bool UseHistoryLegacyPass::runOnFunction(llvm::Function &function) {
  Result = std::make_unique<LLVMHistoryResult>(LLVMHistoryBuilder::build(function));
  return false;
}

void UseHistoryLegacyPass::releaseMemory() { Result.reset(); }

void UseHistoryLegacyPass::getAnalysisUsage(llvm::AnalysisUsage &usage) const {
  usage.setPreservesAll();
}

const LLVMHistoryResult &UseHistoryLegacyPass::getResult() const {
  if (!Result) throw std::logic_error("UseHistory legacy pass has not run");
  return *Result;
}

void registerUseHistoryAnalysis(llvm::FunctionAnalysisManager &manager) {
  manager.registerPass([] { return UseHistoryAnalysis(); });
}

static llvm::RegisterPass<UseHistoryLegacyPass>
    X("usehistory-history", "Use-history analysis", false, true);

} // namespace usehistory
} // namespace lotus
