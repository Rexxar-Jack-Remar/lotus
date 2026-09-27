#include "IR/UseTraceSSA/HistoryPass.h"
#include <llvm/Support/raw_ostream.h>
#include <sstream>
#include <stdexcept>

namespace lotus {
namespace usetracessa {

llvm::AnalysisKey UseTraceSSAAnalysis::Key;
char UseTraceSSALegacyPass::ID = 0;

UseTraceSSAAnalysis::Result UseTraceSSAAnalysis::run(llvm::Function &function,
                                    llvm::FunctionAnalysisManager &) {
  return LLVMHistoryBuilder::build(function);
}

llvm::PreservedAnalyses
UseTraceSSAPrinterPass::run(llvm::Function &function,
                    llvm::FunctionAnalysisManager &manager) {
  const auto &result = manager.getResult<UseTraceSSAAnalysis>(function);
  std::ostringstream text;
  if (Dot) result.graph().printDOT(text);
  else result.graph().print(text);
  Out << text.str();
  return llvm::PreservedAnalyses::all();
}

UseTraceSSALegacyPass::UseTraceSSALegacyPass() : llvm::FunctionPass(ID) {}

bool UseTraceSSALegacyPass::runOnFunction(llvm::Function &function) {
  Result = std::make_unique<LLVMHistoryResult>(LLVMHistoryBuilder::build(function));
  return false;
}

void UseTraceSSALegacyPass::releaseMemory() { Result.reset(); }

void UseTraceSSALegacyPass::getAnalysisUsage(llvm::AnalysisUsage &usage) const {
  usage.setPreservesAll();
}

const LLVMHistoryResult &UseTraceSSALegacyPass::getResult() const {
  if (!Result) throw std::logic_error("UseTraceSSA legacy pass has not run");
  return *Result;
}

void registerUseTraceSSAAnalysis(llvm::FunctionAnalysisManager &manager) {
  manager.registerPass([] { return UseTraceSSAAnalysis(); });
}

static llvm::RegisterPass<UseTraceSSALegacyPass>
    X("usetracessa-history", "Use-history analysis", false, true);

} // namespace usetracessa
} // namespace lotus
